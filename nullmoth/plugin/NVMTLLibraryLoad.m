/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include <os/lock.h>
#include <xlocale.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#import "NVMTLObjects.h"
#include "nvmtl_spec_fc.h"
#include <dlfcn.h>
#include <stdatomic.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
extern char **environ;

void nvlog(const char *fmt, ...);

#import <CommonCrypto/CommonDigest.h>
// The out-of-process AIR decoder: nvmtl-air-dis (plugin/air_dis.c) beside this plugin, built on the same upstream LLVM as the
// in-process reader (air-runtime/libLLVM.dylib). Earlier packages ran a copy of the Metal toolchain's air-opt from
// /Library/GPUBundles/nvmtl; that program is not ours to redistribute and is no longer looked for.
static const char *nvmtl_air_dis_path(void) {
    static char path[PATH_MAX]; static dispatch_once_t once;
    dispatch_once(&once, ^{
        Dl_info info;
        if (dladdr((const void *)&nvmtl_air_dis_path, &info) && info.dli_fname) {
            NSString *p = [[@(info.dli_fname) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"nvmtl-air-dis"];
            strlcpy(path, p.fileSystemRepresentation, sizeof path);
        }
    });
    return path[0] ? path : NULL;
}
#include <unistd.h>
static inline const char *nvmtl_pick_ll(const char *staged, const char *dev) {
    return access(staged, R_OK) == 0 ? staged : dev;
}
#ifndef NVMTL_XLATE_LIB
#define NVMTL_XLATE_LIB nvmtl_companion("libnvmtl_translate.dylib")
#endif

typedef int (*nvmtl_translate_fn)(const char *ll, const char *stage, uint8_t **out, size_t *out_len, char *err, size_t err_len);
typedef int (*nvmtl_fc_fn)(const char *ll, const char *stage, const uint32_t *idx, const uint32_t *sizes,
                           const uint8_t *const *payloads, size_t n, uint8_t **out, size_t *out_len, char *err, size_t err_len);
typedef int (*nvmtl_refl_fn)(const char *ll, const char *stage, uint8_t **out, size_t *out_len, char *err, size_t err_len);
typedef int (*nvmtl_refl_fc_fn)(const char *ll, const char *stage,
                                const uint32_t *idx, const uint32_t *sizes, const uint8_t **payloads, size_t n,
                                uint8_t **out, size_t *out_len, char *err, size_t err_len);
typedef void (*nvmtl_free_fn)(uint8_t *p, size_t len);

typedef int (*nvmtl_lower_fn)(const uint8_t *, size_t, uint8_t **, size_t *, char *, size_t);
static nvmtl_lower_fn xlate_lower;
typedef int (*nvmtl_lower_ex_fn)(const uint8_t *, size_t, uint32_t, uint8_t **, size_t *, uint32_t *, char *, size_t);
static nvmtl_lower_ex_fn xlate_lower_ex;
typedef int (*nvmtl_to_uniform_fn)(const uint8_t *, size_t, uint8_t **, size_t *, char *, size_t);
static nvmtl_to_uniform_fn xlate_to_uniform;   /* the table of an unlowered module moved to a Uniform block */
static nvmtl_translate_fn xlate;
#include "nvmtl_spvcache_name.h"
// The cache tag: the translator build (sha256-<64 hex>, 71 characters) plus every mode suffix. This buffer was 80 bytes,
// so a tag with -u2 and -p5 had room for only "-a" of "-as1", and any suffix after it was silently dropped: entries
// translated with and without the switches that follow shared one name, and a lookup could be served a module made in
// the other mode. nvmtl_spvcache_mode_tag refuses a tag that does not fit instead of cutting it.
static char gXlateTag[NVMTL_SPVCACHE_TAG_MAX] = "untagged";
// The translator build alone, without the mode suffixes (-u2, -p5, ...) gXlateTag adds: the stale-entry sweep keys on it.
static char gXlateBuildTag[NVMTL_SPVCACHE_TAG_MAX] = "untagged";
static void nvmtl_translator_cache_tag(const char *path, char *tag, size_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    CC_SHA256_CTX context; CC_SHA256_Init(&context);
    BOOL complete = fd >= 0; uint8_t buffer[65536];
    if (fd >= 0) {
        for (;;) {
            ssize_t n = read(fd, buffer, sizeof buffer);
            if (n > 0) { CC_SHA256_Update(&context, buffer, (CC_LONG)n); continue; }
            if (n == 0) break;
            if (errno == EINTR) continue;
            complete = NO; break;
        }
        close(fd);
    }
    if (complete) {
        uint8_t digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(digest, &context);
        char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
        for (size_t i = 0; i < sizeof digest; ++i) snprintf(hex + 2*i, 3, "%02x", digest[i]);
        snprintf(tag, capacity, "sha256-%s", hex);
    } else {
        snprintf(tag, capacity, "private-%d-%llx", getpid(),
                 (unsigned long long)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW));
        nvlog("translator cache: content read failed; using process-private identity");
    }
}
typedef int (*nvmtl_sampler_fn)(const char *, const char *, const char *, uint8_t **, size_t *, char *, size_t);
static nvmtl_sampler_fn xlate_sampler;
typedef void (*nvmtl_tglen_fn)(const uint32_t *, uint32_t);
static nvmtl_tglen_fn xlate_tglen;
typedef uint32_t (*nvmtl_stagein_fn)(const uint32_t *, uint32_t);
static nvmtl_stagein_fn xlate_stagein;
static _Thread_local BOOL g_translate_variant_forced;
static nvmtl_fc_fn xlate_fc;
// nvmtl_translate_fc_spec2 / nvmtl_fc_structural: one translation of a function for every set of values of its
// non-structural function constants (translator wrapper spec_fc.rs)
typedef int (*nvmtl_fc_spec2_fn)(const char *, const char *, const uint32_t *, const uint32_t *, const uint8_t *const *, size_t,
                                 const uint32_t *, size_t, uint32_t *, uint8_t **, size_t *, char *, size_t);
typedef int (*nvmtl_fc_structural_fn)(const char *, uint8_t **, size_t *, char *, size_t);
static nvmtl_fc_spec2_fn xlate_fc_spec2;
static nvmtl_fc_structural_fn xlate_fc_structural;
static nvmtl_refl_fn xlate_refl;
static nvmtl_refl_fc_fn xlate_refl_fc;
static nvmtl_free_fn xlate_free;
typedef int (*nvmtl_vrefs_fn)(const char *, uint8_t **, size_t *, char *, size_t);
typedef int (*nvmtl_link_fn)(const char *, const char *, const char *const *, const char *const *, size_t, uint8_t **, size_t *, char *, size_t);
static nvmtl_vrefs_fn xlate_vrefs;
static nvmtl_link_fn xlate_link;
typedef int (*nvmtl_link_rt_fn)(const char *, const char *, const char *const *, const char *const *, size_t,
                                const char *const *, const char *const *, size_t, uint8_t **, size_t *, char *, size_t);
static nvmtl_link_rt_fn xlate_link_rt;
typedef int (*nvmtl_link_ext_fn)(const char *, const char *const *, size_t, uint8_t **, size_t *, char *, size_t);
static nvmtl_link_ext_fn xlate_link_ext;
typedef int (*nvmtl_lower_mesh_fn)(const char *, const char *, uint8_t **, size_t *, uint8_t **, size_t *, uint32_t *, char *, size_t);
static nvmtl_lower_mesh_fn xlate_lower_mesh;
typedef int (*nvmtl_lower_mesh2_fn)(const char *, const char *, const char *, const char *, uint32_t, uint32_t,
    uint8_t **, size_t *, uint8_t **, size_t *, uint8_t **, size_t *, uint32_t *, char *, size_t);
static nvmtl_lower_mesh2_fn xlate_lower_mesh2;
static char kNVMTLTableFns;

// NVMTL_XLATE_DUMP=<dir>: every translation that runs (a cache miss) leaves its input as <dir>/<n>-<stage>.ll and a
// line "<n> <stage> <ms> <input bytes> <output bytes> <entry point>" in <dir>/times.tsv - a corpus of what an app's
// first launch translates, for the translator's speed work
static void nvmtl_xlate_dump(const char *what, const char *ll, const char *stage, uint64_t t0, size_t outn)
{
    static const char *dir = (const char *)-1; if (dir == (const char *)-1) { dir = getenv("NVMTL_XLATE_DUMP"); if (dir && !*dir) dir = NULL; }
    if (!dir || !ll) return;
    static _Atomic unsigned k; const unsigned i = atomic_fetch_add(&k, 1);
    const double ms = (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6;
    char fp[700]; snprintf(fp, sizeof fp, "%s/%d-%05u-%s.ll", dir, (int)getpid(), i, stage ? stage : "x");
    FILE *f = fopen(fp, "w"); if (f) { fputs(ll, f); fclose(f); }
    snprintf(fp, sizeof fp, "%s/times.tsv", dir);
    f = fopen(fp, "a"); if (f) { fprintf(f, "%d-%05u\t%s\t%s\t%.2f\t%zu\t%zu\n", (int)getpid(), i, what, stage ? stage : "x", ms, strlen(ll), outn); fclose(f); }
}
// NVMTL_HITCH_MS: the translator's entry points are timed into the hitch log ("translate")
static nvmtl_translate_fn h_xlate; static nvmtl_fc_fn h_xlate_fc; static nvmtl_link_fn h_xlate_link;
static int nvh_xlate(const char *ll, const char *stage, uint8_t **out, size_t *out_len, char *err, size_t err_len)
{ const uint64_t t = nvmtl_hitch_t0(), t1 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); int r = h_xlate(ll, stage, out, out_len, err, err_len); nvmtl_hitch_add(NVH_XLATE, t, 0);
  nvmtl_xlate_dump("translate", ll, stage, t1, r ? 0 : *out_len); return r; }
static int nvh_xlate_fc(const char *ll, const char *stage, const uint32_t *idx, const uint32_t *sizes,
                        const uint8_t *const *payloads, size_t n, uint8_t **out, size_t *out_len, char *err, size_t err_len)
{ const uint64_t t = nvmtl_hitch_t0(), t1 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); int r = h_xlate_fc(ll, stage, idx, sizes, payloads, n, out, out_len, err, err_len); nvmtl_hitch_add(NVH_XLATE, t, 0);
  nvmtl_xlate_dump("translate_fc", ll, stage, t1, r ? 0 : *out_len); return r; }
static int nvh_xlate_link(const char *ll, const char *stage, const char *const *syms, const char *const *mls, size_t n,
                          uint8_t **out, size_t *out_len, char *err, size_t err_len)
{ const uint64_t t = nvmtl_hitch_t0(); int r = h_xlate_link(ll, stage, syms, mls, n, out, out_len, err, err_len); nvmtl_hitch_add(NVH_XLATE, t, 0); return r; }

static int nvmtl_xlate_ready(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Dl_info info; NSString *bundled = nil;
        if (dladdr((const void *)&nvmtl_xlate_ready, &info) && info.dli_fname)
            bundled = [[@(info.dli_fname) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"libnvmtl_translate.dylib"];
        const char *xpath = bundled && access(bundled.fileSystemRepresentation,R_OK)==0 ? bundled.fileSystemRepresentation : NVMTL_XLATE_LIB;
        void *h = dlopen(xpath, RTLD_NOW | RTLD_LOCAL);
        if (h) nvmtl_translator_cache_tag(xpath, gXlateTag, sizeof gXlateTag);
        strlcpy(gXlateBuildTag, gXlateTag, sizeof gXlateBuildTag);
        // Each mode below changes the SPIR-V a module translates to, so a cache entry written in one mode must never be
        // served in another: unrolled AIR, argument-buffer samplers, and the translator's two diagnostic switches
        // (NVMTL_NO_PTR_PHI_LOAD, NVMTL_NO_PIXEL_HW_FILTER). In the default modes the tag is sha256-<translator>-u2, as
        // before, so caches (and tools/b580/cache-retag.sh) keep working. Canonical pointers (-p5, upstream 460e6e8) are
        // not in this tree.
        { const char *as = getenv("NVMTL_ARGBUF_SAMPLERS");
          if (h && !nvmtl_spvcache_mode_tag(gXlateTag, sizeof gXlateTag, gXlateBuildTag, !getenv("NVMTL_NO_AIR_UNROLL"),
                                       0, as && !strcmp(as, "1"),
                                       getenv("NVMTL_NO_PTR_PHI_LOAD") != NULL, getenv("NVMTL_NO_PIXEL_HW_FILTER") != NULL)) {
              // A tag that cannot name every mode cannot keep the modes apart: no cache entry is read or written.
              strlcpy(gXlateTag, "untagged", sizeof gXlateTag);
              nvlog("translate: cache tag does not fit - the shader cache is OFF in this process");
          }
          // NVMTL_SPARSE_TEXTURES=1: sparse_color::resident() becomes a residency query (translator caps bit 3), so its
          // modules are kept apart from the ones that answer "resident"
          if (h && nvmtl_sparse_knob() && strcmp(gXlateTag, "untagged") && strlcat(gXlateTag, "-sr", sizeof gXlateTag) >= sizeof gXlateTag) {
              strlcpy(gXlateTag, "untagged", sizeof gXlateTag);
              nvlog("translate: cache tag does not fit - the shader cache is OFF in this process");
          } }
        // NVMTL_ICB_DGC=1 lowers the ICB commands into 1024-byte records (translator render_command/dgc.rs): another translation
        if (h && nvmtl_icb_dgc_knob() && strcmp(gXlateTag, "untagged") && strlcat(gXlateTag, "-icbdgc", sizeof gXlateTag) >= sizeof gXlateTag) {
            strlcpy(gXlateTag, "untagged", sizeof gXlateTag);
            nvlog("translate: cache tag does not fit - the shader cache is OFF in this process");
        }
        if (!h) { nvlog("translate: dlopen(%s): %s", NVMTL_XLATE_LIB, dlerror()); return; }
        xlate = (nvmtl_translate_fn)dlsym(h, "nvmtl_translate");
        { char tid[1200];
          nvlog("translate: opened %s (cache tag %s)%s", nvmtl_image_ident((const void *)xlate, tid, sizeof tid), gXlateTag,
                xlate ? "" : " - it has NO nvmtl_translate"); }
        xlate_lower = (nvmtl_lower_fn)dlsym(h, "nvmtl_lower_buffer_addresses");
        xlate_lower_ex = (nvmtl_lower_ex_fn)dlsym(h, "nvmtl_lower_buffer_addresses_ex");
        xlate_to_uniform = (nvmtl_to_uniform_fn)dlsym(h, "nvmtl_table_to_uniform");
        if (!xlate_to_uniform) nvlog("translate: no nvmtl_table_to_uniform in this translator - a module that address lowering refuses is refused outright while the table is Uniform");
        xlate_free = (nvmtl_free_fn)dlsym(h, "nvmtl_translate_free");
        xlate_sampler = (nvmtl_sampler_fn)dlsym(h, "nvmtl_translate_samplers");
        xlate_tglen = (nvmtl_tglen_fn)dlsym(h, "nvmtl_translate_set_threadgroup_lengths");
        xlate_stagein = (nvmtl_stagein_fn)dlsym(h, "nvmtl_translate_set_kernel_stage_in");
        if (!xlate_tglen) nvlog("translate: no nvmtl_translate_set_threadgroup_lengths in this translator — dynamic threadgroup memory stays unsized (MPS convolutions read zeros)");
        xlate_fc = (nvmtl_fc_fn)dlsym(h, "nvmtl_translate_fc");
        xlate_fc_spec2 = (nvmtl_fc_spec2_fn)dlsym(h, "nvmtl_translate_fc_spec2");
        xlate_fc_structural = (nvmtl_fc_structural_fn)dlsym(h, "nvmtl_fc_structural");
        if (!xlate_fc_spec2 || !xlate_fc_structural)
            nvlog("translate: no nvmtl_translate_fc_spec2 in this translator - every function-constant variant is translated whole");
        xlate_vrefs = (nvmtl_vrefs_fn)dlsym(h, "nvmtl_visible_refs");
        xlate_link = (nvmtl_link_fn)dlsym(h, "nvmtl_link_visible");
        xlate_link_rt = (nvmtl_link_rt_fn)dlsym(h, "nvmtl_link_runtime");
        if (!xlate_link_rt) nvlog("translate: no nvmtl_link_runtime in this translator - visible function tables cannot be linked");
        xlate_link_ext = (nvmtl_link_ext_fn)dlsym(h, "nvmtl_link_externs");
        xlate_lower_mesh = (nvmtl_lower_mesh_fn)dlsym(h, "nvmtl_lower_mesh");
        if (!xlate_lower_mesh) nvlog("translate: no nvmtl_lower_mesh in this translator - mesh pipelines will be refused");
        xlate_lower_mesh2 = (nvmtl_lower_mesh2_fn)dlsym(h, "nvmtl_lower_mesh2");
        if (!xlate_lower_mesh2) nvlog("translate: no nvmtl_lower_mesh2 in this translator - object stages, indirect mesh draws and mesh constants will be refused");
        if (!xlate_link_ext) nvlog("translate: no nvmtl_link_externs in this translator - dynamic-library externs cannot be linked");
        if (!xlate_link) nvlog("translate: no nvmtl_link_visible in this translator — kernels with visible-function references cannot be linked");
        xlate_refl = (nvmtl_refl_fn)dlsym(h, "nvmtl_reflect");
        xlate_refl_fc = (nvmtl_refl_fc_fn)dlsym(h, "nvmtl_reflect_fc");
        void (*setcaps)(uint32_t) = (void (*)(uint32_t))dlsym(h, "nvmtl_translate_set_caps");
        // Bits 0/1: formatless storage images. Bit 2: this plugin has no rasterization rate maps. Bit 3: sparse textures
        // (NVMTL_SPARSE_TEXTURES=1; the knob alone decides, since this may run while the device is being made - the
        // device enables shaderResourceResidency with it wherever it has image residency).
        if (setcaps) setcaps(3u | 4u | (nvmtl_sparse_knob() ? 8u : 0u)); else nvlog("translate: no nvmtl_translate_set_caps in this translator — written textures stay R32f-declared (one channel)");
        if (!xlate) { nvlog("translate: no nvmtl_translate symbol"); return; }
        if (nvmtl_hitch_ms() || getenv("NVMTL_XLATE_DUMP")) {
            h_xlate = xlate; xlate = nvh_xlate;
            if (xlate_fc) { h_xlate_fc = xlate_fc; xlate_fc = nvh_xlate_fc; }
            if (xlate_link) { h_xlate_link = xlate_link; xlate_link = nvh_xlate_link; }
        }
        nvlog("translate: in-process AIR->SPIR-V library loaded");
    });
    return xlate != NULL;
}

#ifndef NVMTL_AIRCACHE_DIR
#define NVMTL_AIRCACHE_DIR "/Library/GPUBundles/nvmtl/aircache"
#endif

static _Atomic unsigned long gAirHit, gAirMiss, gAirStored;

// prod9b: the cache folders are chosen again after a store is refused. A process that the system sandboxes after the
// plugin has loaded (Safari's GPU process on macOS 26: com.apple.WebKit.GPU enters its sandbox after +load) passed the
// write probe beforehand and then had every store refused ("STORE FAILED ... You don't have permission"), so each new
// GPU process translated everything again and WebKit killed it as unresponsive. The sandbox lets such a process write
// only below its own folder in the user cache directory (DARWIN_USER_CACHE_DIR/<process name>, where Metal keeps
// com.apple.metalfe): that folder is now a candidate, probed after the refusal.
static BOOL nvmtl_dir_writable(NSString *d);
// Cache stores in a sandboxed process (Safari's WebKit GPU process on macOS 26): its profile lets open, write, close and
// rename through in its own cache and temporary folders, but not fsync, F_FULLFSYNC or renamex_np. NSData's writes call
// those after the bytes are on disk and then report a failure (the atomic one blocks), so every store of that process
// counted as refused and it never kept a cache: CoreAnimation's specialized shaders took 2 s each again on every launch.
// A store is written here with the calls the profile allows: a uniquely named temporary file beside the target,
// written in full, closed, then renamed into place (a reader sees the old file or the whole new one, never a part).
static BOOL nvmtl_store_file(NSString *path, NSData *data) {
    if (!path.length || !data) return NO;
    static _Atomic unsigned seq;
    NSString *tmp = [path stringByAppendingFormat:@".%d.%u.tmp", getpid(), atomic_fetch_add(&seq, 1)];
    const int fd = open(tmp.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return NO;
    const uint8_t *b = data.bytes; size_t left = data.length; BOOL ok = YES;
    while (left && ok) {
        const ssize_t n = write(fd, b, left);
        if (n > 0) { b += n; left -= (size_t)n; } else if (n < 0 && errno == EINTR) continue; else ok = NO;
    }
    if (close(fd) != 0) ok = NO;
    if (ok && rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation) != 0) ok = NO;
    if (!ok) unlink(tmp.fileSystemRepresentation);
    return ok;
}
static BOOL nvmtl_store_text(NSString *path, NSString *text) { return nvmtl_store_file(path, [text dataUsingEncoding:NSUTF8StringEncoding]); }
// ---- shader-cache entries that prove they are whole (nm29: nullmoth 52a62bf, nvmtl_cache_file.h) ----
// Translated-shader, specialization, reflection, link and variant entries are written as payload+footer (magic, length,
// 64-bit hash) through the same temporary-file-and-rename as nvmtl_store_file (no fsync: the WebKit GPU sandbox refuses
// it), and read back only when the footer agrees: a torn, truncated, flipped or older-format file is a miss and is removed,
// never a shader. Nothing is stored once the device is lost in this process.
#include "nvmtl_cache_file.h"
int nvmtl_vk_backend_lost(void);
static _Atomic unsigned long gCacheRefused, gCacheSkippedLost;
static NSData *nvmtl_cache_read(NSString *path) {
    if (!path.length) return nil;
    size_t n = 0; nvmtl_cf_status st = NVMTL_CF_MISSING;
    uint8_t *b = nvmtl_cf_read(path.fileSystemRepresentation, &n, &st);
    if (!b) {
        if (st != NVMTL_CF_MISSING) {
            const unsigned long k = atomic_fetch_add(&gCacheRefused, 1) + 1;
            if (!(k & (k - 1))) nvlog("shader cache: refused and removed %s: %s [%lu refused so far]", path.lastPathComponent.UTF8String, nvmtl_cf_why(st), k);
        }
        return nil;
    }
    return [NSData dataWithBytesNoCopy:b length:n freeWhenDone:YES];
}
static BOOL nvmtl_cache_store(NSString *path, NSData *d) {
    if (!path.length || !d) return NO;
    if (nvmtl_vk_backend_lost()) {
        const unsigned long k = atomic_fetch_add(&gCacheSkippedLost, 1) + 1;
        if (!(k & (k - 1))) nvlog("shader cache: not writing %s - the device is lost in this process [%lu skipped]", path.lastPathComponent.UTF8String, k);
        return NO;
    }
    return nvmtl_cf_write(path.fileSystemRepresentation, d.bytes, d.length) == 0;
}
static os_unfair_lock gCacheDirLock = OS_UNFAIR_LOCK_INIT;
static _Atomic int gCacheGen;
static NSString *gAirDir, *gSpvDir;
static int gAirGen = -1, gSpvGen = -1;
static NSString *nvmtl_user_cache_dir(BOOL own) {
    char ucd[PATH_MAX];
    const size_t n = confstr(_CS_DARWIN_USER_CACHE_DIR, ucd, sizeof ucd);
    if (!(n > 0 && n <= sizeof ucd)) return nil;
    NSString *d = @(ucd);
    return own ? [d stringByAppendingPathComponent:@(getprogname())] : d;
}
// prod9f: a folder that refused a store is not chosen again in this process. Safari's GPU process logged 57 refusals in
// 50 minutes: every refusal chose the folders again and got the same one back (the system folder is taken on access(),
// which the sandbox does not answer), so it never kept a cache and translated every page's shaders again. A refused
// folder goes into gRefusedDirs and the choosers skip it; when every candidate has refused, stores stop for the process
// (reads go on from the folder already chosen).
static NSMutableSet<NSString *> *gRefusedDirs;    // under gCacheDirLock
static _Atomic int gAirStoresOff, gSpvStoresOff;
static BOOL nvmtl_dir_refused(NSString *d) { return d.length && [gRefusedDirs containsObject:d]; }   // lock held
static inline BOOL nvmtl_air_stores_on(void) { return !atomic_load_explicit(&gAirStoresOff, memory_order_relaxed); }
static inline BOOL nvmtl_spv_stores_on(void) { return !atomic_load_explicit(&gSpvStoresOff, memory_order_relaxed); }
static void nvmtl_cache_store_refused(const char *what, NSString *path) {
    NSString *dir = path.stringByDeletingLastPathComponent;
    if (!dir.length) return;
    os_unfair_lock_lock(&gCacheDirLock);
    BOOL fresh = NO;
    if (![gRefusedDirs containsObject:dir]) {   // stores still in flight in a refused folder do not choose again
        if (!gRefusedDirs) gRefusedDirs = [NSMutableSet new];
        [gRefusedDirs addObject:dir]; fresh = YES;
    }
    const int g = fresh ? ++gCacheGen : gCacheGen;
    os_unfair_lock_unlock(&gCacheDirLock);
    static _Atomic unsigned long n;
    const unsigned long k = atomic_fetch_add(&n, 1) + 1;
    if (fresh || k <= 3 || !(k & (k - 1)))   // a new folder always; repeats in one the first few, then powers of two
        nvlog("cache: a %s store was refused in %s (%lu so far)%s (generation %d)", what, dir.UTF8String, k,
              fresh ? " - choosing the cache folders again without it" : "", g);
}
static NSString *nvmtl_aircache_dir(void) {
    os_unfair_lock_lock(&gCacheDirLock);
    const int gen = gCacheGen;
    if (gAirDir && gAirGen == gen) { NSString *d = gAirDir; os_unfair_lock_unlock(&gCacheDirLock); return d; }
    NSString *prev = gAirDir, *dir = nil;
    BOOL off = NO;
    const char *e = getenv("NVMTL_AIRCACHE");
    if (e) { dir = @(e); off = nvmtl_dir_refused(dir); }
    // nm29 (nullmoth 400111c): the system folder must take a real write (nvmtl_dir_writable: our temp-file-and-rename
    // store) from the first choice on. access(W_OK) answers yes for a folder a sandbox then refuses to open for writing
    // (WindowServer's sandbox allows writes only inside its own named folders), and the first stores of every process were
    // refused before the folders were chosen again.
    else if (!nvmtl_dir_refused(@NVMTL_AIRCACHE_DIR) && nvmtl_dir_writable(@NVMTL_AIRCACHE_DIR)) dir = @NVMTL_AIRCACHE_DIR;
    else {
        NSArray *c = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
        NSString *ucd = nvmtl_user_cache_dir(NO), *own = nvmtl_user_cache_dir(YES);
        NSArray<NSString *> *cand = @[
            [(c.count ? c[0] : NSTemporaryDirectory()) stringByAppendingPathComponent:@"nvmtl/aircache"],
            ucd ? [ucd stringByAppendingPathComponent:@"nvmtl/aircache"] : @"",
            own ? [own stringByAppendingPathComponent:@"nvmtl/aircache"] : @"",
            [NSTemporaryDirectory() stringByAppendingPathComponent:@"nvmtl/aircache"]];
        for (NSString *d in cand) if (d.length && !nvmtl_dir_refused(d) && nvmtl_dir_writable(d)) { dir = d; break; }
        if (!dir) {
            off = nvmtl_dir_refused(cand.lastObject);   // the fallback has refused too: no folder left to store in
            dir = off ? (prev ?: cand.lastObject) : cand.lastObject;
        }
    }
    gAirDir = dir; gAirGen = gen;
    const BOOL wasOff = atomic_exchange(&gAirStoresOff, off);
    os_unfair_lock_unlock(&gCacheDirLock);
    if (off && !wasOff) nvlog("aircache: every cache folder refused a store - AIR text is not stored in this process (reads go on from %s)", dir.UTF8String);
    else if (prev && ![prev isEqualToString:dir]) nvlog("aircache: %s refused a store - caching in %s", prev.UTF8String, dir.UTF8String);
    return dir;
}
static _Atomic unsigned long gSpvHit, gSpvMiss, gSpvStored;
static _Atomic unsigned long gFcHit, gFcMiss, gFcStored;

static _Atomic unsigned long long gSpvAdded;
static _Atomic int gSpvTrimRunning;
static _Atomic int gSpvTrimArmed = 1;

typedef struct { long long use; unsigned long long size; } nvmtl_spvent;
static int nvmtl_spvent_older(const void *a, const void *b) {
    long long x = ((const nvmtl_spvent *)a)->use, y = ((const nvmtl_spvent *)b)->use;
    return (x > y) - (x < y);
}

static unsigned long long nvmtl_spvcache_rearm_bytes(void) {
    static unsigned long long rearm; static dispatch_once_t o;
    dispatch_once(&o, ^{
        rearm = 256ull * 1024 * 1024;
        const char *r = getenv("NVMTL_SPVCACHE_REARM_MB");
        if (r) rearm = strtoull(r, NULL, 10) * 1024ull * 1024;
    });
    return rearm;
}

static void nvmtl_spvcache_trim_now(const char *dirc) {
    unsigned long long cap = 2048ull * 1024 * 1024;
    const char *e = getenv("NVMTL_SPVCACHE_MAX_MB");
    if (e) { unsigned long long v = strtoull(e, NULL, 10); if (!v) { nvlog("spvcache: trim disabled by NVMTL_SPVCACHE_MAX_MB=0"); return; } cap = v * 1024ull * 1024; }
    unsigned long long pct = 80;
    { const char *t = getenv("NVMTL_SPVCACHE_TARGET_PCT");
      if (t) { unsigned long long v = strtoull(t, NULL, 10); if (v >= 10 && v <= 99) pct = v; } }
    unsigned long long target = cap / 100 * pct;

    DIR *dh = opendir(dirc);
    if (!dh) { nvlog("spvcache: trim CANNOT READ %s: %s - cache NOT bounded this process", dirc, strerror(errno)); return; }
    size_t room = 4096, n = 0; unsigned long skipped = 0;
    nvmtl_spvent *ents = malloc(room * sizeof *ents);
    unsigned long long total = 0;
    struct dirent *de; char p[PATH_MAX];
    while ((de = readdir(dh))) {
        if (de->d_name[0] == '.') continue;
        if (snprintf(p, sizeof p, "%s/%s", dirc, de->d_name) >= (int)sizeof p) { skipped++; continue; }
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) { skipped++; continue; }
        total += (unsigned long long)st.st_size;
        if (!ents) continue;
        if (n == room) {
            nvmtl_spvent *r = realloc(ents, room * 2 * sizeof *ents);
            if (!r) { free(ents); ents = NULL; continue; }
            ents = r; room *= 2;
        }
        ents[n].use = (long long)st.st_mtime; ents[n].size = (unsigned long long)st.st_size; n++;
    }
    closedir(dh);
    if (!ents) { nvlog("spvcache: trim ABANDONED - out of memory listing %s (%llu MiB counted so far)", dirc, total >> 20); return; }

    if (total <= cap) {
        nvlog("spvcache: %llu.%llu MiB in %lu files (%lu unreadable), under the %llu MiB cap - nothing trimmed",
              total >> 20, ((total & 0xFFFFFull) * 10) >> 20, (unsigned long)n, skipped, cap >> 20);
        free(ents); return;
    }
    qsort(ents, n, sizeof *ents, nvmtl_spvent_older);
    unsigned long long need = total - target;
    unsigned long long slack = need + need / 16;
    unsigned long long acc = 0; long long cutoff = ents[0].use; unsigned long plan = 0;
    for (size_t i = 0; i < n; i++) { acc += ents[i].size; cutoff = ents[i].use; plan = (unsigned long)(i + 1); if (acc >= slack) break; }
    free(ents);

    nvlog("spvcache: %llu.%llu MiB in %lu files (%lu unreadable) over the %llu MiB cap - TRIMMING oldest-use-first to %llu MiB (%llu%%): need %llu.%llu MiB, up to %lu files at last-use <= %lld",
          total >> 20, ((total & 0xFFFFFull) * 10) >> 20, (unsigned long)n, skipped, cap >> 20,
          target >> 20, pct, need >> 20, ((need & 0xFFFFFull) * 10) >> 20, plan, cutoff);

    dh = opendir(dirc);
    if (!dh) { nvlog("spvcache: trim ABANDONED before unlinking - reopen %s: %s - NOTHING was deleted", dirc, strerror(errno)); return; }
    unsigned long long freed = 0; unsigned long gone = 0, failed = 0; int last = 0;
    while ((de = readdir(dh))) {
        if (freed >= need) break;
        if (de->d_name[0] == '.') continue;
        if (snprintf(p, sizeof p, "%s/%s", dirc, de->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if ((long long)st.st_mtime > cutoff) continue;
        if (unlink(p) == 0) { freed += (unsigned long long)st.st_size; gone++; }
        else { failed++; last = errno; }
    }
    closedir(dh);
    unsigned long long remain = total > freed ? total - freed : 0;
    nvlog("spvcache: trimmed %lu files / %llu.%llu MiB of %llu.%llu MiB needed, %lu unlinks failed (%s); %llu.%llu MiB of the walked set remain against a %llu MiB target%s",
          gone, freed >> 20, ((freed & 0xFFFFFull) * 10) >> 20,
          need >> 20, ((need & 0xFFFFFull) * 10) >> 20,
          failed, failed ? strerror(last) : "none",
          remain >> 20, ((remain & 0xFFFFFull) * 10) >> 20, target >> 20,
          freed >= need ? "" : " - SHORT, TARGET NOT REACHED");
}

static void nvmtl_spvcache_trim_async(NSString *dir) {
    unsigned long long rearm = nvmtl_spvcache_rearm_bytes();
    if (!atomic_load_explicit(&gSpvTrimArmed, memory_order_relaxed) &&
        (rearm == 0 || atomic_load_explicit(&gSpvAdded, memory_order_relaxed) < rearm)) return;
    if (atomic_exchange(&gSpvTrimRunning, 1)) return;
    atomic_store(&gSpvTrimArmed, 0);
    atomic_store(&gSpvAdded, 0);
    NSString *dirCopy = [dir copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
      locale_t nvPrevLocale = uselocale(LC_C_LOCALE);
      @try {
        char dirc[PATH_MAX];
        if ([dirCopy getFileSystemRepresentation:dirc maxLength:sizeof dirc]) nvmtl_spvcache_trim_now(dirc);
        else nvlog("spvcache: trim ABANDONED - no fs representation for the cache directory");
      } @catch (NSException *x) {
        nvlog("spvcache: trim ABANDONED for this process - %s: %s (patch_fsrep.py)",
              x.name.UTF8String ?: "?", x.reason.UTF8String ?: "?");
      }
      atomic_store(&gSpvTrimRunning, 0);
      uselocale(nvPrevLocale);
    });
}

// The SPIR-V cache folder is chosen once, by creating it and writing to it. Sandboxed agents (iconservicesagent) could
// not create the folder beside their AIR cache; every store failed with "folder doesn't exist" and every launch
// translated its shaders again. The process's own temporary folder is always writable inside its sandbox.
static BOOL nvmtl_dir_writable(NSString *d) {
    if (!d || ![[NSFileManager defaultManager] createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil]) return NO;
    // prod9e: the probe is written as the stores are (data, atomically): an empty file needs only file-write-create,
    // and Safari's GPU process sandbox (macOS 26) let that through where every real store was refused
    // prod9f: and as the record stores are (a uniquely named temporary file, written, then renamed beside it): the
    // atomic write alone still passed in Safari's GPU process while the stores kept being refused
    static _Atomic unsigned probeSeq;
    const unsigned seq = atomic_fetch_add(&probeSeq, 1);
    NSString *probe = [d stringByAppendingPathComponent:[NSString stringWithFormat:@".probe-%d-%u.spv", getpid(), seq]];
    static const char kProbe[] = "nvmtl cache probe";
    NSData *bytes = [NSData dataWithBytes:kProbe length:sizeof kProbe];
    // written twice, as a new entry and as a replacement, the way the stores write (nvmtl_store_file)
    BOOL ok = nvmtl_store_file(probe, bytes) && nvmtl_store_file(probe, bytes);
    unlink(probe.fileSystemRepresentation);
    return ok;
}
// A cache entry's name carries the build of the translator that made it, so after a driver update none of the older
// entries can ever be found again: they are dead weight, and the first login after an update pays to translate
// everything from scratch while they sit there. The folder is swept once per process, on a background queue, so no
// frame waits for it.
// Measured on one machine: the window server's cache held 2,672 entries from seven translator builds (709 MB together
// with the AIR cache) of which 372 were usable, and the login after an update spent 107 s at 79% CPU in translation
// with nothing on screen. The size cap had never noticed, because the total stayed below it.
// The sweep must not run before the translator has been opened. Until then the build tag is "untagged", nothing would
// match it, and a cache the running driver can use would be deleted.
static void nvmtl_spvcache_drop_stale(const char *dirc) {
    if (!dirc || !strcmp(gXlateBuildTag, "untagged")) {
        nvlog("spvcache: stale-tag sweep SKIPPED - the translator has no cache tag yet; nothing was deleted");
        return;
    }
    // Every process shares this folder unless it names its own cache, and each process's modes (canonical pointers,
    // argument-buffer samplers, an application profile's translator switches) give its entries a different tag. Only
    // entries from another translator BUILD are stale; an entry of the same build in another mode belongs to a process
    // that runs in that mode, and deleting it made each process wipe the others' caches on every start.

    DIR *eh = opendir(dirc);
    if (!eh) {
        nvlog("spvcache: stale-tag sweep CANNOT READ %s: %s - entries from older drivers are left in place",
              dirc, strerror(errno));
        return;
    }
    unsigned long gone = 0, kept = 0, failed = 0;
    unsigned long long freed = 0;
    const struct dirent *de;
    while ((de = readdir(eh)) != NULL) {
        const size_t n = strlen(de->d_name);
        if (n < 7 || strcmp(de->d_name + n - 7, ".c3.spv") != 0) continue;   /* not one of ours */
        if (nvmtl_spvcache_same_build(de->d_name, gXlateBuildTag)) { kept++; continue; }
        char full[1200];
        if (snprintf(full, sizeof full, "%s/%s", dirc, de->d_name) >= (int)sizeof full) continue;
        struct stat st;
        const unsigned long long sz = (stat(full, &st) == 0) ? (unsigned long long)st.st_size : 0;
        if (unlink(full) == 0) { gone++; freed += sz; } else failed++;
    }
    closedir(eh);
    if (gone || failed)
        nvlog("spvcache: dropped %lu entries / %llu MiB left by earlier drivers (%lu of build %s kept, %lu could not be removed)",
              gone, freed >> 20, kept, gXlateBuildTag, failed);
}
static NSString *nvmtl_spvcache_dir(void) {
    NSString *air = nvmtl_aircache_dir();   // first: it may itself be chosen again
    os_unfair_lock_lock(&gCacheDirLock);
    const int gen = gCacheGen;
    if (gSpvGen == gen) { NSString *d = gSpvDir; os_unfair_lock_unlock(&gCacheDirLock); return d; }
    NSString *prev = gSpvDir, *dir = nil;
    NSString *beside = [[air stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"spvcache"];
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:@"nvmtl/spvcache"];
    // WindowServer's sandbox allows writes only inside its own named folders (…/C/WindowServer, …/T/WindowServer):
    // both folders above refused it, so it translated every CoreAnimation shader again on each boot (N-89, Tahoe).
    NSString *ownBase = nvmtl_user_cache_dir(YES);
    NSString *own = ownBase ? [ownBase stringByAppendingPathComponent:@"nvmtl/spvcache"] : nil;
    BOOL off = NO;
    if (!nvmtl_dir_refused(beside) && nvmtl_dir_writable(beside)) dir = beside;
    else if (!nvmtl_dir_refused(own) && nvmtl_dir_writable(own)) dir = own;
    else if (!nvmtl_dir_refused(tmp) && nvmtl_dir_writable(tmp)) dir = tmp;
    else if (prev) { dir = prev; off = nvmtl_dir_refused(prev); }   // every folder refused: read on, store nothing
    gSpvDir = dir; gSpvGen = gen;
    const BOOL wasOff = atomic_exchange(&gSpvStoresOff, off);
    os_unfair_lock_unlock(&gCacheDirLock);
    if (off) { if (!wasOff) nvlog("spvcache: every cache folder refused a store - translated shaders are not stored in this process (reads go on from %s)", dir.UTF8String); }
    else if (dir && ![dir isEqualToString:beside]) nvlog("spvcache: %s is not writable here - caching in %s", beside.UTF8String, dir.UTF8String);
    else if (!dir) nvlog("spvcache: no writable folder in this process (%s, %s) - translated shaders are not cached", beside.UTF8String, tmp.UTF8String);
    (void)prev;
    return dir;
}
static NSString *nvmtl_spvcache_path_digest(const unsigned char h[CC_SHA256_DIGEST_LENGTH], NSString *stage) {
    // No tag, no file: an entry named without its modes could be served to a process in another mode.
    if (!strcmp(gXlateTag, "untagged")) return nil;
    NSString *d = nvmtl_spvcache_dir();
    if (!d) return nil;
    // Sweep the folder clear of other builds' entries once, here rather than where the folder is chosen: a process
    // that picks its folder before the translator is open does not know the build yet and would skip the sweep for
    // good. The window server does exactly that, so its cache kept seven builds' entries. By the time a path is
    // being built the build tag is real, because the name below is made from it.
    static dispatch_once_t sweepOnce;
    if (!getenv("NVMTL_SPVCACHE_KEEP_STALE")) dispatch_once(&sweepOnce, ^{
        NSString *chosen = d;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            char dirc[1024];
            if ([chosen getFileSystemRepresentation:dirc maxLength:sizeof dirc]) nvmtl_spvcache_drop_stale(dirc);
        });
    });
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    nvmtl_spvcache_trim_async(d);
    return [d stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.%@.%s.c3.spv", hex, stage, gXlateTag]];
}
static NSString *nvmtl_spvcache_path(NSData *air, NSString *stage) {
    unsigned char h[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(air.bytes, (CC_LONG)air.length, h);
    return nvmtl_spvcache_path_digest(h, stage);
}

/* The cache key of an AIR module with a short tail (function constants, sampler states ...) is SHA-256(size || air || tail).
 * The module is megabytes of text and the tail a few hundred bytes. VideoToolbox's Metal scaler makes its function again
 * on every transfer, so the same module was converted to UTF-8 and hashed three times per dispatch (function, reflection,
 * variant). Hash the module once, keep the hash state on the NSString that holds it, and finish each key from that state:
 * the digests, and so the file names, are the ones the one-shot hash gave. */
@interface NVMTLAirDigest : NSObject { @public CC_SHA256_CTX _ctx; }
@end
@implementation NVMTLAirDigest
@end
static const void *NVMTL_AIRDIGEST_KEY = &NVMTL_AIRDIGEST_KEY;
static const void *NVMTL_AIRTEXT_KEY = &NVMTL_AIRTEXT_KEY;
// Marks text nvmtl_air_to_text made; it is never edited, so state can be kept on it.
/* OpenCL's native_* math (native_powr, native_exp, ...) reaches AIR as air.native_<f>, which the translator does not know:
   Geekbench 7's HDR kernels (correct_brightness, resolve_radiance) failed translation and their workloads errored
   (2026-10-28). native_ asks for implementation precision, which is what air.fast_<f> lowers to, so the names are
   rewritten to that family, declarations and calls alike, in the text every translator entry is handed. native_divide
   and native_recip have no fast_ form and stay. */
// (OpenCL's native_* math is renamed to air.fast_* by the translator itself: native::lower_air_text, every entry point.)
static NSString *nvmtl_air_text_made(NSString *s) {
    if (s.length >= 4096) objc_setAssociatedObject(s, NVMTL_AIRTEXT_KEY, @YES, OBJC_ASSOCIATION_RETAIN);
    return s;
}
static NVMTLAirDigest *nvmtl_air_state(NSString *ll) {
    if (!ll) return nil;
    // Only long strings we marked: every CF-backed string is a kind of NSMutableString, so a class test keeps nothing.
    const BOOL cacheable = ll.length >= 4096 && objc_getAssociatedObject(ll, NVMTL_AIRTEXT_KEY) != nil;
    NVMTLAirDigest *d = cacheable ? objc_getAssociatedObject(ll, NVMTL_AIRDIGEST_KEY) : nil;
    if (d) return d;
    NSData *air = [ll dataUsingEncoding:NSUTF8StringEncoding];
    if (!air) return nil;
    d = [NVMTLAirDigest new];
    uint64_t size = air.length;
    CC_SHA256_Init(&d->_ctx);
    CC_SHA256_Update(&d->_ctx, &size, sizeof size);
    for (NSUInteger off = 0; off < air.length; off += 1u << 30)
        CC_SHA256_Update(&d->_ctx, (const char *)air.bytes + off, (CC_LONG)MIN((NSUInteger)1u << 30, air.length - off));
    if (cacheable) objc_setAssociatedObject(ll, NVMTL_AIRDIGEST_KEY, d, OBJC_ASSOCIATION_RETAIN);
    return d;
}
static NSString *nvmtl_spvcache_path_air(NVMTLAirDigest *ad, NSData *tail, NSString *stage) {
    CC_SHA256_CTX c = ad->_ctx;
    unsigned char h[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Update(&c, tail.bytes, (CC_LONG)tail.length);
    CC_SHA256_Final(h, &c);
    return nvmtl_spvcache_path_digest(h, stage);
}
/* Cache-file contents kept in memory, by file name. The name is the content digest plus the translator tag, so an entry is the
 * answer for that exact input and translator; it is dropped by NSCache under memory pressure. */
static BOOL nvmtl_is_spirv(NSData *d) {
    uint32_t magic = 0;
    if (d.length < 20 || d.length % 4) return NO;
    memcpy(&magic, d.bytes, 4);
    return magic == 0x07230203;
}
static NSCache *nvmtl_memo(void) {
    static NSCache *c; static dispatch_once_t once;
    dispatch_once(&once, ^{ c = [NSCache new]; c.totalCostLimit = 96u << 20; c.countLimit = 256; });
    return c;
}

static BOOL nvmtl_air_unroll_wanted(NSData *air);
static NSString *nvmtl_aircache_path(NSData *air) {
    unsigned char h[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(air.bytes, (CC_LONG)air.length, h);
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    return [[nvmtl_aircache_dir() stringByAppendingPathComponent:@(hex)] stringByAppendingPathExtension:
            nvmtl_air_unroll_wanted(air) ? @"u2.ll" : @"ll"];
}

static BOOL nvmtl_run_air_opt(const char *input, const char *output) {
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) return NO;
    rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t child = -1;
    const char *tool = nvmtl_air_dis_path();
    if (!tool) { posix_spawn_file_actions_destroy(&actions); nvlog("library: nvmtl-air-dis not found beside the plugin"); return NO; }
    char *argv[] = { (char *)tool, "-S", (char *)input, "-o", (char *)output, NULL };
    // an empty environment: the tool needs none, and the caller's DYLD_ and other variables are not handed on
    char *noenv[] = { NULL };
    if (!rc) rc = posix_spawn(&child, tool, &actions, NULL, argv, noenv);
    posix_spawn_file_actions_destroy(&actions);
    if (rc) { nvlog("library: nvmtl-air-dis spawn failed: %s", strerror(rc)); return NO; }
    uint64_t deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) + 20ull * 1000000000ull;
    int status = 0;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return YES;
            nvlog("library: nvmtl-air-dis failed (wait status %d)", status); return NO;
        }
        if (result < 0 && errno != EINTR) {
            nvlog("library: nvmtl-air-dis wait failed: %s", strerror(errno)); return NO;
        }
        if (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            nvlog("library: nvmtl-air-dis exceeded 20-second limit"); return NO;
        }
        usleep(10000);
    }
}

static void *(*nvLLVMContextCreate)(void);
static void (*nvLLVMContextDispose)(void *);
static void (*nvLLVMContextSetDiagnosticHandler)(void *, void (*)(void *, void *), void *);
static void *(*nvLLVMCreateMemoryBufferWithMemoryRangeCopy)(const char *, size_t, const char *);
static void (*nvLLVMDisposeMemoryBuffer)(void *);
static int (*nvLLVMParseBitcodeInContext2)(void *, void *, void **);
static char *(*nvLLVMPrintModuleToString)(void *);
static void (*nvLLVMDisposeModule)(void *);
static void (*nvLLVMDisposeMessage)(char *);
static char *(*nvLLVMGetDiagInfoDescription)(void *);
static void *(*nvLLVMRunPasses)(void *, const char *, void *, void *);
static void *(*nvLLVMCreatePassBuilderOptions)(void);
static void (*nvLLVMDisposePassBuilderOptions)(void *);
static char *(*nvLLVMGetErrorMessage)(void *);
static void (*nvLLVMDisposeErrorMessage)(char *);
struct nvmtl_llvm_walk {
    void *(*GetFirstGlobal)(void *); void *(*GetNextGlobal)(void *); void *(*GlobalGetValueType)(void *);
    void *(*GetFirstUse)(void *); void *(*GetNextUse)(void *); void *(*GetUser)(void *);
    void *(*IsAConstantExpr)(void *); int (*GetConstOpcode)(void *); void *(*GetGEPSourceElementType)(void *);
    int (*GetNumOperands)(void *); void *(*GetOperand)(void *, unsigned); void *(*IsAConstantInt)(void *);
    long long (*ConstIntGetSExtValue)(void *); int (*GetTypeKind)(void *); unsigned (*GetIntTypeWidth)(void *);
    void *(*GetElementType)(void *); unsigned long long (*GetArrayLength2)(void *); void *(*GetModuleContext)(void *);
    void *(*Int64TypeInContext)(void *); void *(*ConstInt)(void *, unsigned long long, int);
    void *(*ConstInBoundsGEP2)(void *, void *, void **, unsigned); void (*ReplaceAllUsesWith)(void *, void *);
};
typedef struct nvmtl_llvm_walk nvmtl_llvm_walk;
#define NVMTL_LLVM_WALK_NAMES "LLVMGetFirstGlobal", "LLVMGetNextGlobal", "LLVMGlobalGetValueType", "LLVMGetFirstUse", \
    "LLVMGetNextUse", "LLVMGetUser", "LLVMIsAConstantExpr", "LLVMGetConstOpcode", "LLVMGetGEPSourceElementType", \
    "LLVMGetNumOperands", "LLVMGetOperand", "LLVMIsAConstantInt", "LLVMConstIntGetSExtValue", "LLVMGetTypeKind", \
    "LLVMGetIntTypeWidth", "LLVMGetElementType", "LLVMGetArrayLength2", "LLVMGetModuleContext", "LLVMInt64TypeInContext", \
    "LLVMConstInt", "LLVMConstInBoundsGEP2", "LLVMReplaceAllUsesWith"
static int nvmtl_scalar_bytes(const nvmtl_llvm_walk *w, void *t) {
    switch (w->GetTypeKind(t)) {
    case 1: case 18: return 2; case 2: return 4; case 3: return 8;
    case 8: { unsigned b = w->GetIntTypeWidth(t); return (b == 8 || b == 16 || b == 32 || b == 64) ? (int)(b / 8) : 0; }
    default: return 0;
    }
}
static int nvmtl_retype_byte_geps(const nvmtl_llvm_walk *w, void *module) {
    void *i64 = w->Int64TypeInContext(w->GetModuleContext(module)); int n = 0;
    for (void *g = w->GetFirstGlobal(module); g; g = w->GetNextGlobal(g)) {
        void *vt = w->GlobalGetValueType(g);
        if (w->GetTypeKind(vt) != 11) continue;
        void *et = w->GetElementType(vt); int sz = nvmtl_scalar_bytes(w, et);
        if (sz <= 1) continue;
        unsigned long long len = w->GetArrayLength2(vt);
        void *hits[256]; void *repl[256]; int nh = 0;
        for (void *u = w->GetFirstUse(g); u && nh < 256; u = w->GetNextUse(u)) {
            void *ce = w->GetUser(u);
            if (!w->IsAConstantExpr(ce) || w->GetConstOpcode(ce) != 29 ) continue;
            if (w->GetNumOperands(ce) != 2 || w->GetOperand(ce, 0) != g) continue;
            void *src = w->GetGEPSourceElementType(ce);
            if (w->GetTypeKind(src) != 8 || w->GetIntTypeWidth(src) != 8) continue;
            void *ix = w->GetOperand(ce, 1); if (!w->IsAConstantInt(ix)) continue;
            long long b = w->ConstIntGetSExtValue(ix);
            if (b < 0 || b % sz || (unsigned long long)(b / sz) > len) continue;
            void *idx[2] = { w->ConstInt(i64, 0, 0), w->ConstInt(i64, (unsigned long long)(b / sz), 0) };
            hits[nh] = ce; repl[nh] = w->ConstInBoundsGEP2(vt, g, idx, 2); nh++;
        }
        for (int k = 0; k < nh; k++) if (repl[k] && repl[k] != hits[k]) { w->ReplaceAllUsesWith(hits[k], repl[k]); n++; }
    }
    return n;
}
static int nvmtl_llvm_walk_load(nvmtl_llvm_walk *w, void *library) {
    static const char *names[] = { NVMTL_LLVM_WALK_NAMES }; void **slot = (void **)w;
    for (unsigned k = 0; k < sizeof names / sizeof *names; k++) if (!(slot[k] = dlsym(library, names[k]))) return 0;
    return 1;
}
static nvmtl_llvm_walk gLLVMWalk;

// Set only when the reader reports an ERROR about a module - it used to only log, so a module the reader
// had rejected was still compiled. Severity matters: the same handler carries warnings, remarks and notes,
// and treating those as fatal would refuse shaders that compile perfectly well today. The severity symbol
// is loaded optionally, so a libLLVM without it keeps the old behaviour rather than disabling the reader.
static int (*nvLLVMGetDiagInfoSeverity)(void *);
static _Thread_local int gAirError;
static void nvmtl_llvm_diagnostic(void *diagnostic, void *context) {
    (void)context;
    if (nvLLVMGetDiagInfoSeverity && nvLLVMGetDiagInfoSeverity(diagnostic) == 0) gAirError = 1;   // LLVMDSError
    char *message = nvLLVMGetDiagInfoDescription(diagnostic);
    if (message) { nvlog("library: bitcode reader: %s", message); nvLLVMDisposeMessage(message); }
}
static BOOL nvmtl_bitcode_reader_ready(void) {
    static dispatch_once_t once; static BOOL ready;
    dispatch_once(&once, ^{
        Dl_info info;
        if (!dladdr((const void *)&nvmtl_bitcode_reader_ready, &info) || !info.dli_fname) return;
        NSString *path = [[@(info.dli_fname) stringByDeletingLastPathComponent]
                         stringByAppendingPathComponent:@"air-runtime/libLLVM.dylib"];
        void *library = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
        if (!library) { nvlog("library: in-memory bitcode reader unavailable: %s", dlerror()); return; }
#define NV_LLVM_LOAD(name) do { nv##name = (void *)dlsym(library, #name); if (!nv##name) { nvlog("library: bitcode reader missing " #name); return; } } while (0)
        NV_LLVM_LOAD(LLVMContextCreate);
        NV_LLVM_LOAD(LLVMContextDispose);
        NV_LLVM_LOAD(LLVMContextSetDiagnosticHandler);
        NV_LLVM_LOAD(LLVMCreateMemoryBufferWithMemoryRangeCopy);
        NV_LLVM_LOAD(LLVMDisposeMemoryBuffer);
        NV_LLVM_LOAD(LLVMParseBitcodeInContext2);
        NV_LLVM_LOAD(LLVMPrintModuleToString);
        NV_LLVM_LOAD(LLVMDisposeModule);
        NV_LLVM_LOAD(LLVMDisposeMessage);
        NV_LLVM_LOAD(LLVMGetDiagInfoDescription);
        // optional: absent in some builds, and its absence must not disable the reader
        nvLLVMGetDiagInfoSeverity = (int (*)(void *))dlsym(library, "LLVMGetDiagInfoSeverity");
        if (!nvLLVMGetDiagInfoSeverity) nvlog("library: bitcode reader has no LLVMGetDiagInfoSeverity - reader errors are logged only");
#undef NV_LLVM_LOAD
        nvLLVMRunPasses = dlsym(library, "LLVMRunPasses"); nvLLVMCreatePassBuilderOptions = dlsym(library, "LLVMCreatePassBuilderOptions");
        nvLLVMDisposePassBuilderOptions = dlsym(library, "LLVMDisposePassBuilderOptions");
        nvLLVMGetErrorMessage = dlsym(library, "LLVMGetErrorMessage"); nvLLVMDisposeErrorMessage = dlsym(library, "LLVMDisposeErrorMessage");
        if (!nvLLVMRunPasses || !nvLLVMCreatePassBuilderOptions || !nvLLVMDisposePassBuilderOptions || !nvLLVMGetErrorMessage || !nvLLVMDisposeErrorMessage) {
            nvLLVMRunPasses = NULL; nvlog("library: air unroll unavailable - libLLVM lacks the pass-builder API"); }
        if (nvLLVMRunPasses && !nvmtl_llvm_walk_load(&gLLVMWalk, library)) {
            nvLLVMRunPasses = NULL; nvlog("library: air unroll unavailable - libLLVM lacks the IR-walk API the byte-GEP retype needs"); }
        ready = YES;
        nvlog("library: in-memory bitcode reader ready");
    });
    return ready;
}
#define NVMTL_AIR_UNROLL_PIPELINE "function(sroa,loop(loop-rotate),indvars,loop-unroll<O3>,sroa,simplifycfg,early-cse)"
static BOOL nvmtl_air_unroll_wanted(NSData *air) {
    static int off = -1; if (off < 0) off = getenv("NVMTL_NO_AIR_UNROLL") != NULL;
    static const char needle[] = "air.simdgroup_matrix_8x8";
    return !off && air.length && memmem(air.bytes, air.length, needle, sizeof needle - 1) != NULL;
}
// The largest shader library measured in a real app is a few megabytes. The reader we hand this to is
// LLVM's, which its own documentation declines to call hardened against malicious input, so cap the blob
// before it gets there: a library over the cap falls through to the out-of-process path below, where a
// crash costs a failed translation instead of the app.
#define NVMTL_AIR_MAX_BYTES (64u * 1024u * 1024u)
static NSString *nvmtl_air_in_memory(NSData *air) {
    if (!nvmtl_bitcode_reader_ready()) return nil;
    if (air.length < 4 || air.length > NVMTL_AIR_MAX_BYTES) {
        nvlog("library: bitcode blob is %lu bytes - outside [4, %u], not read in process",
              (unsigned long)air.length, NVMTL_AIR_MAX_BYTES);
        return nil;
    }
    void *context = nvLLVMContextCreate();
    if (!context) return nil;
    nvLLVMContextSetDiagnosticHandler(context, nvmtl_llvm_diagnostic, NULL);
    void *buffer = nvLLVMCreateMemoryBufferWithMemoryRangeCopy(air.bytes, air.length, "nvmtl-air");
    void *module = NULL; NSString *result = nil;
    gAirError = 0;
    if (buffer && !nvLLVMParseBitcodeInContext2(context, buffer, &module) && module && !gAirError) {
        if (nvLLVMRunPasses && nvmtl_air_unroll_wanted(air)) {
            void *opts = nvLLVMCreatePassBuilderOptions();
            void *err = nvLLVMRunPasses(module, NVMTL_AIR_UNROLL_PIPELINE, NULL, opts);
            nvLLVMDisposePassBuilderOptions(opts);
            if (err) { char *m = nvLLVMGetErrorMessage(err); nvlog("library: air unroll FAILED (%s) - translating as decoded", m ? m : "?"); if (m) nvLLVMDisposeErrorMessage(m); }
            else { int rt = nvmtl_retype_byte_geps(&gLLVMWalk, module);
                   nvlog("library: air unroll: simdgroup_matrix module (%lu bytes) unrolled + sroa, %d byte GEPs retyped", (unsigned long)air.length, rt); }
        }
        char *text = nvLLVMPrintModuleToString(module);
        if (text) { result = [NSString stringWithUTF8String:text]; nvLLVMDisposeMessage(text); }
    }
    if (module) nvLLVMDisposeModule(module);
    if (buffer) nvLLVMDisposeMemoryBuffer(buffer);
    nvLLVMContextDispose(context);
    return result;
}

static NSString *nvmtl_air_to_text(NSData *air) {
    NSString *cache = nvmtl_aircache_path(air);
    NSString *hit = [NSString stringWithContentsOfFile:cache encoding:NSUTF8StringEncoding error:nil];
    if (hit.length) { gAirHit++; return nvmtl_air_text_made(hit); }
    gAirMiss++;
    if (nvmtl_hitch_ms()) nvmtl_hitch_add_ns(NVH_SPVMISS, 0);
    NSString *memoryIR = nvmtl_air_in_memory(air);
    if (memoryIR.length) {
        if (!nvmtl_air_stores_on()) {}
        else if (nvmtl_store_text(cache, memoryIR)) gAirStored++; else nvmtl_cache_store_refused("aircache", cache);
        return nvmtl_air_text_made(memoryIR);
    }

    NSString *dir = NSTemporaryDirectory();
    NSString *inp = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"nvmtl-%d-%p.air", getpid(), air]];
    NSString *outp = [inp stringByAppendingPathExtension:@"ll"];
    if (![air writeToFile:inp atomically:NO]) { nvlog("library: cannot write %s", inp.UTF8String); return nil; }
    if (!nvmtl_run_air_opt(inp.fileSystemRepresentation, outp.fileSystemRepresentation)) {
        unlink(inp.fileSystemRepresentation); unlink(outp.fileSystemRepresentation); return nil;
    }
    NSString *ll = [NSString stringWithContentsOfFile:outp encoding:NSUTF8StringEncoding error:nil];
    unlink(inp.UTF8String); unlink(outp.UTF8String);
    if (ll.length && nvmtl_air_stores_on()) {
        NSError *mkerr = nil;
        [[NSFileManager defaultManager] createDirectoryAtPath:nvmtl_aircache_dir()
                                  withIntermediateDirectories:YES attributes:nil error:&mkerr];
        if (nvmtl_store_text(cache, ll)) gAirStored++; else nvmtl_cache_store_refused("aircache", cache);
    }
    return nvmtl_air_text_made(ll);
}

static BOOL nvmtl_entry_of(NSString *ll, NSString **name, NSString **stage) {
    for (NSString *s in @[ @"vertex", @"fragment", @"kernel", @"mesh", @"object" ]) {
        NSString *key = [NSString stringWithFormat:@"!air.%@ = !{!", s];
        NSRange k = [ll rangeOfString:key];
        if (k.location == NSNotFound) continue;
        NSRange rest = NSMakeRange(NSMaxRange(k), ll.length - NSMaxRange(k));
        NSRange end = [ll rangeOfString:@"}" options:0 range:rest];
        if (end.location == NSNotFound) continue;
        NSString *node = [ll substringWithRange:NSMakeRange(rest.location, end.location - rest.location)];
        NSRange nc = [node rangeOfString:@","];
        if (nc.location != NSNotFound) node = [node substringToIndex:nc.location];
        NSString *decl = [NSString stringWithFormat:@"\n!%@ = !{", node];
        NSRange d = [ll rangeOfString:decl];
        if (d.location == NSNotFound) continue;
        NSRange nl = [ll rangeOfString:@"\n" options:0 range:NSMakeRange(NSMaxRange(d), ll.length - NSMaxRange(d))];
        const NSUInteger stop = (nl.location == NSNotFound) ? ll.length : nl.location;
        NSRange dr = NSMakeRange(NSMaxRange(d), stop - NSMaxRange(d));
        NSRange at = [ll rangeOfString:@"@" options:0 range:dr];
        if (at.location == NSNotFound) continue;
        NSRange after = NSMakeRange(NSMaxRange(at), stop - NSMaxRange(at));
        NSRange comma = [ll rangeOfString:@"," options:0 range:after];
        NSRange brace = [ll rangeOfString:@"}" options:0 range:after];
        NSUInteger nameEnd = stop;
        if (comma.location != NSNotFound) nameEnd = MIN(nameEnd, comma.location);
        if (brace.location != NSNotFound) nameEnd = MIN(nameEnd, brace.location);
        if (nameEnd <= after.location) continue;
        *name = [ll substringWithRange:NSMakeRange(after.location, nameEnd - after.location)];
        if ((*name).length >= 2 && [*name hasPrefix:@"\""] && [*name hasSuffix:@"\""])
            *name = [*name substringWithRange:NSMakeRange(1, (*name).length - 2)];
        *stage = s;
        return YES;
    }
    return NO;
}

static NSArray<NSString *> *nvmtl_visible_of(NSString *ll)
{
    NSMutableDictionary<NSString *, NSString *> *node = [NSMutableDictionary new];
    NSMutableSet<NSString *> *defined = [NSMutableSet new];
    NSMutableArray<NSString *> *ids = [NSMutableArray new];
    [ll enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
        if ([line hasPrefix:@"define"]) {
            NSRange at = [line rangeOfString:@"@"];
            if (at.location == NSNotFound) return;
            NSUInteger s = at.location + 1, e;
            BOOL q = (s < line.length && [line characterAtIndex:s] == '"');
            if (q) s++;
            for (e = s; e < line.length; e++) { unichar c = [line characterAtIndex:e];
                if (q ? (c == '"') : (c == '(')) break; }
            if (e > s) [defined addObject:[line substringWithRange:NSMakeRange(s, e - s)]];
            return;
        }
        if (![line hasPrefix:@"!"]) return;
        if ([line hasPrefix:@"!air.visible = "] || [line hasPrefix:@"!air.ci = "] || [line hasPrefix:@"!air.intersection = "]) {
            NSRange eq = [line rangeOfString:@"="];
            for (NSString *tok in [[line substringFromIndex:eq.location] componentsSeparatedByString:@"!"]) {
                NSUInteger d = 0;
                while (d < tok.length) { unichar c = [tok characterAtIndex:d]; if (c < '0' || c > '9') break; d++; }
                if (d) [ids addObject:[tok substringToIndex:d]];
            }
            return;
        }
        NSRange eq = [line rangeOfString:@" = !{"];
        if (eq.location == NSNotFound || eq.location < 2) return;
        NSString *idn = [line substringWithRange:NSMakeRange(1, eq.location - 1)];
        NSRange at = [line rangeOfString:@"@" options:0 range:NSMakeRange(eq.location, line.length - eq.location)];
        if (at.location == NSNotFound) return;
        NSUInteger s = at.location + 1, e;
        BOOL q = (s < line.length && [line characterAtIndex:s] == '"');
        if (q) s++;
        for (e = s; e < line.length; e++) { unichar c = [line characterAtIndex:e];
            if (q ? (c == '"') : (c == ',' || c == ')' || c == '}' || c == ' ')) break; }
        if (e > s) node[idn] = [line substringWithRange:NSMakeRange(s, e - s)];
    }];
    NSMutableArray<NSString *> *out = [NSMutableArray new];
    for (NSString *i in ids) { NSString *nm = node[i]; if (nm && [defined containsObject:nm]) [out addObject:nm]; }
    return out;
}

static NSString *nvmtl_sym_at(NSString *line)
{
    NSRange at = [line rangeOfString:@"@"];
    if (at.location == NSNotFound) return nil;
    NSUInteger i = at.location + 1, n = line.length;
    if (i < n && [line characterAtIndex:i] == '"') {
        NSRange q = [line rangeOfString:@"\"" options:0 range:NSMakeRange(i + 1, n - i - 1)];
        if (q.location == NSNotFound) return nil;
        return [line substringWithRange:NSMakeRange(i + 1, q.location - i - 1)];
    }
    NSUInteger j = i;
    while (j < n) {
        unichar c = [line characterAtIndex:j];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '$')) break;
        j++;
    }
    return j > i ? [line substringWithRange:NSMakeRange(i, j - i)] : nil;
}

static NSArray<NSString *> *nvmtl_exported_defs(NSString *ll)
{
    NSMutableArray<NSString *> *out = [NSMutableArray new];
    [ll enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
        if (![line hasPrefix:@"define "]) return;
        NSRange at = [line rangeOfString:@"@"];
        if (at.location == NSNotFound) return;
        NSString *head = [line substringToIndex:at.location];
        for (NSString *k in @[@" internal ", @" private ", @" hidden ", @" available_externally "])
            if ([head rangeOfString:k].location != NSNotFound) return;
        NSString *s = nvmtl_sym_at(line);
        if (s.length && ![s hasPrefix:@"air."] && ![s hasPrefix:@"llvm."]) [out addObject:s];
    }];
    return out;
}

NSArray<NSString *> *nvmtl_externs_uncached(NSDictionary<NSString *, NSString *> *airs)
{
    if (!airs.count) return @[];
    NSMutableSet<NSString *> *declared = [NSMutableSet new], *defined = [NSMutableSet new];
    NSMutableSet *seen = [NSMutableSet new];
    for (NSString *fn in airs) {
        NSString *ll = airs[fn];
        if (!ll) continue;
        NSValue *key = [NSValue valueWithPointer:(__bridge const void *)ll];
        if ([seen containsObject:key]) continue;
        [seen addObject:key];
        [ll enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
            BOOL isDecl = [line hasPrefix:@"declare"];
            if (!isDecl && ![line hasPrefix:@"define"]) return;
            NSString *sym = nvmtl_sym_at(line);
            if (!sym.length || [sym hasPrefix:@"air."] || [sym hasPrefix:@"llvm."]) return;
            [(isDecl ? declared : defined) addObject:sym];
        }];
    }
    [declared minusSet:defined];
    return declared.allObjects;
}

NSSet<NSString *> *nvmtl_mtlb_extern_names_uncached(NSData *raw)
{
    NSMutableSet<NSString *> *out = [NSMutableSet new];
    const uint8_t *b = raw.bytes; NSUInteger n = raw.length;
    if (n < 0x28 || memcmp(b, "MTLB", 4) != 0) return out;
    uint64_t off = 0; memcpy(&off, b + 0x18, 8);
    if (off > n - 4) return out;   /* n >= 0x28 above; off + 4 would wrap for an offset near 2^64 */
    uint32_t count = 0; memcpy(&count, b + off, 4);
    NSUInteger pos = (NSUInteger)off + 4;
    for (uint32_t k = 0; k < count && pos + 4 <= n; k++) {
        uint32_t size = 0; memcpy(&size, b + pos, 4);
        if (size < 4 || pos + size > n) { nvlog("mtlb: function list entry %u runs past the container - stopping", k); break; }
        NSUInteger q = pos + 4, end = pos + size; NSString *name = nil; int type = -1;
        while (q + 4 <= end) {
            const uint8_t *tag = b + q; q += 4;
            if (!memcmp(tag, "ENDT", 4)) break;
            if (q + 2 > end) break;
            uint16_t len = 0; memcpy(&len, b + q, 2); q += 2;
            if (q + len > end) break;
            if (!memcmp(tag, "NAME", 4) && len) name = [[NSString alloc] initWithBytes:b + q length:strnlen((const char *)b + q, len) encoding:NSUTF8StringEncoding];
            else if (!memcmp(tag, "TYPE", 4) && len) type = b[q];
            q += len;
        }
        if (type == 5 && name.length) [out addObject:name];
        pos = end;
    }
    return out;
}
NSSet<NSString *> *nvmtl_mtlb_extern_names(NSData *raw) {
    if (!raw.length) return [NSSet set];
    static char key;
    @synchronized (raw) {
        NSSet *hit = objc_getAssociatedObject(raw, &key); if (hit) return hit;
        NSSet *r = [nvmtl_mtlb_extern_names_uncached(raw) copy];
        objc_setAssociatedObject(raw, &key, r, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return r;
    }
}

static char gExternsKey;
NSArray<NSString *> *nvmtl_externs_of_airs(NSDictionary<NSString *, NSString *> *airs) {
    if(!airs.count)return @[];
    if([airs isKindOfClass:NSMutableDictionary.class])return nvmtl_externs_uncached(airs);
    @synchronized(airs) {
        NSArray *hit=objc_getAssociatedObject(airs,&gExternsKey);if(hit)return hit;
        NSArray *result=nvmtl_externs_uncached(airs);
        objc_setAssociatedObject(airs,&gExternsKey,result,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return result;
    }
}

/* In-flight translations (background/parallel translation). A library's entries are queued for translation on
   background threads when the library is created (nvmtl_bg_translate_library), so that newFunctionWithName: and
   pipeline creation find their SPIR-V in the cache instead of translating on the app's thread (GL programs under Apple's
   GL: 13-22 ms per shader on Minecraft's render thread; Geekbench: ~3 s per cold suite). One entry is translated once:
   a caller that finds it running waits for it; one that finds it still queued takes it over and runs it on its own
   thread (it never waits behind the queue at background priority). NVMTL_BG_XLATE=0: no background translation. */
@interface NVMTLXlateJob : NSObject { @public int state; NSData *spv; NSString *err; } @end   // state 0 queued, 1 running, 2 done
@implementation NVMTLXlateJob @end
static NSCondition *gXlateCond;
static NSMutableDictionary<NSString *, NVMTLXlateJob *> *gXlateJobs;
static _Atomic unsigned long long gBgQueued, gBgDone, gBgTaken, gBgWaited, gBgWaitNs, gBgSkipped;
static __thread int gXlateOnBg;   // this thread is a background translation worker
static void nvmtl_xlate_jobs_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ gXlateCond = [NSCondition new]; gXlateJobs = [NSMutableDictionary new]; });
}
NSData *nvmtl_compile_library_entry(NSData *air, NSString *ll, NSString *stage, NSString *name, NSString **error) {
    if (!air || !ll || !stage || !nvmtl_xlate_ready()) { if(error)*error=@"missing entry or translator"; return nil; }
    if (air.length >= 20 && *(const uint32_t *)air.bytes == 0x07230203u) return air;
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        NSString *spvp = nvmtl_spvcache_path(air, stage);
        NSData *spvData = getenv("NVMTL_SPVCACHE_OFF") ? nil : nvmtl_cache_read(spvp);
        int hit = spvData.length >= 20 && *(const uint32_t *)spvData.bytes == 0x07230203u;
        uint8_t *spv = NULL; size_t spvn = 0; char err[512] = { 0 };
        NVMTLXlateJob *job = nil;
        if (!hit && spvp) {   // one translation per entry: wait for a running one, take over a queued one
            nvmtl_xlate_jobs_init();
            const uint64_t w0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            BOOL waited = NO;
            [gXlateCond lock];
            NVMTLXlateJob *j = gXlateJobs[spvp];
            if (!j) { job = [NVMTLXlateJob new]; job->state = 1; gXlateJobs[spvp] = job; }
            else if (j->state == 0) { job = j; job->state = 1; if (!gXlateOnBg) atomic_fetch_add(&gBgTaken, 1); }
            else {
                waited = YES; while (j->state == 1) [gXlateCond wait];
                // handed back to the queue (a background worker deferred it): take it over now
                if (j->state == 0 && gXlateJobs[spvp] == j) { job = j; job->state = 1; waited = NO; }
            }
            [gXlateCond unlock];
            if (waited && j->state == 2) {
                atomic_fetch_add(&gBgWaited, 1); atomic_fetch_add(&gBgWaitNs, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - w0);
                if (!j->spv && error) *error = j->err ?: @"translation failed";
                return j->spv;
            }
            if (!j) {   // a translation that finished between the cache read and the table: its file is there now
                NSData *again = nvmtl_cache_read(spvp);
                if (again.length >= 20 && *(const uint32_t *)again.bytes == 0x07230203u) {
                    [gXlateCond lock]; job->spv = again; job->state = 2; [gXlateJobs removeObjectForKey:spvp]; [gXlateCond broadcast]; [gXlateCond unlock];
                    gSpvHit++; return again;
                }
            }
        }
        if (hit) { gSpvHit++; utimes(spvp.fileSystemRepresentation, NULL); }
        else {
            gSpvMiss++;
            if (nvmtl_hitch_ms()) nvmtl_hitch_add_ns(NVH_SPVMISS, 0);
            if (xlate(ll.UTF8String, stage.UTF8String, &spv, &spvn, err, sizeof err) != 0) {
                // a background translation of an entry nobody has asked for yet is not a failure the app sees: counted, and
                // its caller (if one ever comes) translates and logs it then
                static _Atomic unsigned long long bgfail;
                if (gXlateOnBg) { const unsigned long long k = atomic_fetch_add(&bgfail, 1) + 1;
                    if (!(k & (k - 1))) nvlog("translate (background): %llu entries the translator refuses, left to their callers (last: %s)", k, name.UTF8String); }
                else nvlog("library: translate %s (%s) FAILED: %s", name.UTF8String, stage.UTF8String, err);
                // NVMTL_XLATE_FAIL_DUMP=<dir>: the text the translator refused, as <dir>/<entry>.ll (translator repros)
                const char *fd = getenv("NVMTL_XLATE_FAIL_DUMP");
                if (fd && *fd) [ll writeToFile:[NSString stringWithFormat:@"%s/%@.ll", fd, name] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
                if (job) { [gXlateCond lock]; job->err = @(err); job->state = 2; [gXlateJobs removeObjectForKey:spvp]; [gXlateCond broadcast]; [gXlateCond unlock]; }
                if (error) *error = @(err); return nil;
            }
            spvData = [NSData dataWithBytes:spv length:spvn];
            { NSError *werr = nil;
              if (spvp && !nvmtl_spv_stores_on()) {}
              else if (spvp && nvmtl_cache_store(spvp, spvData)) {
                  gSpvStored++;
                  atomic_fetch_add_explicit(&gSpvAdded, (unsigned long long)spvData.length, memory_order_relaxed);
              } else if (spvp) {
                  nvmtl_cache_store_refused("spvcache", spvp); nvlog("spvcache: STORE FAILED %s (%zu bytes): %s", spvp.lastPathComponent.UTF8String,
                        (size_t)spvData.length, werr.localizedDescription.UTF8String ?: "?");
              } }
        }
        if (spv && xlate_free) xlate_free(spv, spvn);
        if (job) { [gXlateCond lock]; job->spv = spvData; job->state = 2; [gXlateJobs removeObjectForKey:spvp]; [gXlateCond broadcast]; [gXlateCond unlock]; }
        { double ms = (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6;
          nvtrace("XLATE %s %s %zu B %.1f ms %s", name.UTF8String, stage.UTF8String, (size_t)spvData.length, ms, hit ? "hit" : "miss"); }
        nvlog("library: %s (%s) -> %zu bytes of SPIR-V (%s)", name.UTF8String, stage.UTF8String, (size_t)spvData.length, hit ? "cache hit" : "translated");
        if (getenv("NVMTL_REFLECT") && xlate_refl) {
            uint8_t *j = NULL; size_t jn = 0; char je[512] = { 0 };
            if (xlate_refl(ll.UTF8String, stage.UTF8String, &j, &jn, je, sizeof je) == 0) {
                NSString *js = [[NSString alloc] initWithBytes:j length:jn encoding:NSUTF8StringEncoding];
                NSRange ab = [js rangeOfString:@"argument_buffer_fields"];
                if (ab.location != NSNotFound) {
                    NSUInteger end = MIN(js.length, ab.location + 400);
                    nvlog("reflect %s: %s", name.UTF8String, [[js substringWithRange:NSMakeRange(ab.location, end - ab.location)] UTF8String]);
                } else nvlog("reflect %s: no argument_buffer_fields in %zu bytes of reflection", name.UTF8String, jn);
                if (xlate_free) xlate_free(j, jn);
            } else nvlog("reflect %s FAILED: %s", name.UTF8String, je);
        }
    nvlog("library entry: spvcache %lu hit, %lu miss, %lu stored", gSpvHit, gSpvMiss, gSpvStored);
    return spvData;
}

NSDictionary<NSString *, NSData *> *nvmtl_index_metallib_uncached(NSData *lib, NSDictionary **stageOut, NSDictionary **airOut, NSMutableArray *rec)
{
    if (stageOut) *stageOut = nil;
    if (airOut) *airOut = nil;
    if (!nvmtl_xlate_ready()) return nil;
    NSMutableDictionary<NSString *, NSData *> *out = [NSMutableDictionary dictionary];
    NSMutableDictionary *stages = [NSMutableDictionary new], *airs = [NSMutableDictionary new];
    const uint8_t *b = lib.bytes; NSUInteger n = lib.length, found = 0, visible = 0;
    for (NSUInteger i = 0; i + 20 <= n; i++) {
        if (!(b[i] == 0xde && b[i+1] == 0xc0 && b[i+2] == 0x17 && b[i+3] == 0x0b)) continue;
        uint32_t off = 0, size = 0;
        memcpy(&off, b + i + 8, 4); memcpy(&size, b + i + 12, 4);
        if ((NSUInteger)off + size == 0 || i + off + size > n) continue;
        found++;
        NSData *air = [NSData dataWithBytes:b + i length:off + size];
        NSString *ll = nvmtl_air_to_text(air);
        if (!ll) continue;
        NSString *name = nil, *stage = nil;
        if (!nvmtl_entry_of(ll, &name, &stage)) {
            NSArray<NSString *> *vis = nvmtl_visible_of(ll);
            for (NSString *v in vis) { airs[v] = ll; stages[v] = @"visible"; }
            if (vis.count) [rec addObject:@[@(i), vis, @"visible"]];
            if (vis.count) { visible += vis.count; continue; }
            if (n > 0x0A && memcmp(b, "MTLB", 4) == 0 && b[0x0A] == 2) {
                NSArray<NSString *> *ex = nvmtl_exported_defs(ll);
                for (NSString *v in ex) { airs[v] = ll; stages[v] = @"visible"; }
                if (ex.count) {
                    [rec addObject:@[@(i), ex, @"visible"]];
                    visible += ex.count;
                    nvlog("library: dynamic-library blob %lu exports %lu function(s): %s", (unsigned long)found,
                          (unsigned long)ex.count, [[ex componentsJoinedByString:@","] UTF8String]);
                    continue;
                }
            }
            nvlog("library: blob %lu has no air entry-point metadata and no visible functions", (unsigned long)found);
            continue;
        }
        out[name] = air;
        stages[name] = stage;
        airs[name] = ll;
        [rec addObject:@[@(i), name, stage]];
        i += off + size - 1;
    }
    nvlog("library: metallib %lu bytes, %lu bitcode blobs, %lu entries indexed, %lu visible"
          " | aircache %lu hit, %lu miss, %lu stored | spvcache %lu hit, %lu miss, %lu stored (%s)",
          (unsigned long)n, (unsigned long)found, (unsigned long)out.count, (unsigned long)visible,
          gAirHit, gAirMiss, gAirStored, gSpvHit, gSpvMiss, gSpvStored, nvmtl_aircache_dir().UTF8String);
    if (stageOut) *stageOut = [stages copy];
    if (airOut) *airOut = [airs copy];
    return (out.count || visible) ? [out copy] : nil;
}

static NSString *nvmtl_idx_path(NSData *key) {
    NSMutableString *h = [NSMutableString stringWithString:@"idx2-"]; const uint8_t *d = key.bytes;
    for (NSUInteger i = 0; i < key.length; i++) [h appendFormat:@"%02x", d[i]];
    return [nvmtl_aircache_dir() stringByAppendingPathComponent:[h stringByAppendingString:@".plist"]];
}
static _Atomic unsigned long gLazyLibs, gLazyNames, gLazyForced, gLazyFail;
@interface NVMTLLazyAirs : NSDictionary { NSDictionary *_slot; NSArray *_blob; NSMutableArray *_text; os_unfair_lock _lk; }
- (instancetype)initWithSlots:(NSDictionary *)slot blobs:(NSArray *)blob;
- (NSUInteger)blobBytes;
@end
@implementation NVMTLLazyAirs
- (instancetype)initWithObjects:(const id [])o forKeys:(const id<NSCopying> [])k count:(NSUInteger)n { return self; }
- (instancetype)initWithSlots:(NSDictionary *)slot blobs:(NSArray *)blob {
    if (!(self = [super init])) return nil;
    _slot = [slot copy]; _blob = [blob copy]; _lk = OS_UNFAIR_LOCK_INIT;
    _text = [NSMutableArray arrayWithCapacity:blob.count];
    for (NSUInteger i = 0; i < blob.count; i++) [_text addObject:(id)kCFNull];
    const unsigned long libs = atomic_fetch_add(&gLazyLibs, 1) + 1; atomic_fetch_add(&gLazyNames, slot.count);
    if (!(libs & (libs - 1))) nvlog("lazy air: %lu disk-indexed libraries hold %lu names, %lu texts decoded on first use, %lu failed",
                                    libs, atomic_load(&gLazyNames), atomic_load(&gLazyForced), atomic_load(&gLazyFail));
    return self;
}
- (NSUInteger)count { return _slot.count; }
- (NSEnumerator *)keyEnumerator { return [_slot keyEnumerator]; }
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)st objects:(id __unsafe_unretained [])buf count:(NSUInteger)len {
    return [_slot countByEnumeratingWithState:st objects:buf count:len];
}
- (id)copyWithZone:(NSZone *)z { return self; }
- (NSUInteger)blobBytes { NSUInteger n = 0; for (NSData *d in _blob) n += d.length; return n; }
- (id)objectForKey:(id)key {
    NSNumber *s = _slot[key]; if (!s) return nil;
    const NSUInteger i = s.unsignedIntegerValue;
    os_unfair_lock_lock(&_lk); id t = _text[i]; os_unfair_lock_unlock(&_lk);
    if (t != (id)kCFNull) return t;
    NSString *ll = nvmtl_air_to_text(_blob[i]);
    if (!ll) {
        atomic_fetch_add(&gLazyFail, 1);
        nvlog("lazy air: blob %lu (asked as \"%s\") has no text - the function is absent, as in the uncached index",
              (unsigned long)i, [key description].UTF8String);
        return nil;
    }
    os_unfair_lock_lock(&_lk);
    if (_text[i] == (id)kCFNull) { _text[i] = ll; atomic_fetch_add(&gLazyForced, 1); } else ll = _text[i];
    os_unfair_lock_unlock(&_lk);
    const unsigned long f = atomic_load(&gLazyForced);
    if (!(f & (f - 1))) nvlog("lazy air: %lu texts decoded on first use across %lu disk-indexed libraries holding %lu names (%lu failed)",
                              f, atomic_load(&gLazyLibs), atomic_load(&gLazyNames), atomic_load(&gLazyFail));
    return ll;
}
@end
static NSDictionary *nvmtl_index_from_record(NSData *lib, NSDictionary *rec, NSDictionary **stageOut, NSDictionary **airOut, NSArray **extOut) {
    NSArray *blobs = rec[@"blobs"], *ext = rec[@"externs"];
    if (![blobs isKindOfClass:NSArray.class] || ![ext isKindOfClass:NSArray.class] || ![rec[@"length"] isEqual:@(lib.length)] || !blobs.count) return nil;
    NSMutableDictionary *out = [NSMutableDictionary new], *stages = [NSMutableDictionary new], *airs = [NSMutableDictionary new];
    const uint8_t *b = lib.bytes; NSUInteger n = lib.length, visible = 0;
    const int lazy = !getenv("NVMTL_NO_LAZYAIR"); NSMutableArray *blob = [NSMutableArray new];
    const int nocopy = !getenv("NVMTL_NO_NOCOPY");
    static _Atomic int nocopySaid;
    if (nocopy && !atomic_exchange(&nocopySaid, 1)) nvlog("library index: disk-indexed blobs reference the container bytes (no copy; NVMTL_NO_NOCOPY=1 copies)");
    for (NSArray *e in blobs) {
        if (![e isKindOfClass:NSArray.class] || e.count != 3 || ![e[0] isKindOfClass:NSNumber.class]) return nil;
        NSUInteger i = [e[0] unsignedIntegerValue];
        if (i + 20 > n || !(b[i] == 0xde && b[i+1] == 0xc0 && b[i+2] == 0x17 && b[i+3] == 0x0b)) return nil;
        uint32_t off = 0, size = 0; memcpy(&off, b + i + 8, 4); memcpy(&size, b + i + 12, 4);
        if ((NSUInteger)off + size == 0 || i + off + size > n) return nil;
        NSData *air = nocopy ? [[NSData alloc] initWithBytesNoCopy:(void *)(b + i) length:off + size deallocator:^(void *p, NSUInteger l) { (void)lib; }]
                             : [NSData dataWithBytes:b + i length:off + size];
        id ll = nil;
        if (lazy) { ll = @(blob.count); [blob addObject:air]; }
        else { ll = nvmtl_air_to_text(air); if (!ll) return nil; }
        if ([e[1] isKindOfClass:NSArray.class]) { for (NSString *v in e[1]) { airs[v] = ll; stages[v] = @"visible"; visible++; } }
        else if ([e[1] isKindOfClass:NSString.class] && [e[2] isKindOfClass:NSString.class]) { out[e[1]] = air; stages[e[1]] = e[2]; airs[e[1]] = ll; }
        else return nil;
    }
    if (!out.count && !visible) return nil;
    *stageOut = [stages copy]; *extOut = ext;
    *airOut = lazy ? [[NVMTLLazyAirs alloc] initWithSlots:airs blobs:blob] : [airs copy];
    return [out copy];
}
static NSDictionary *nvmtl_index_metallib_disk(NSData *lib, NSData *key, NSDictionary **stageOut, NSDictionary **airOut) {
    NSString *p = getenv("NVMTL_IDXCACHE_OFF") ? nil : nvmtl_idx_path(key);
    NSData *d = p ? [NSData dataWithContentsOfFile:p] : nil;
    if (d) {
        NSDictionary *rec = [NSPropertyListSerialization propertyListWithData:d options:0 format:NULL error:NULL];
        NSDictionary *st = nil, *ai = nil; NSArray *ext = nil;
        NSDictionary *fns = [rec isKindOfClass:NSDictionary.class] ? nvmtl_index_from_record(lib, rec, &st, &ai, &ext) : nil;
        if (fns) {
            objc_setAssociatedObject(ai, &gExternsKey, ext, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            nvlog("library index: DISK hit bytes=%lu entries=%lu names=%lu externs=%lu", (unsigned long)lib.length, (unsigned long)fns.count, (unsigned long)ai.count, (unsigned long)ext.count);
            if (stageOut) *stageOut = st; if (airOut) *airOut = ai;
            return fns;
        }
        nvlog("library index: disk record %s does not validate against this library - rebuilding it", p.lastPathComponent.UTF8String);
    }
    NSMutableArray *rec = [NSMutableArray new]; NSDictionary *st = nil, *ai = nil;
    NSDictionary *fns = nvmtl_index_metallib_uncached(lib, &st, &ai, rec);
    if (stageOut) *stageOut = st; if (airOut) *airOut = ai;
    if (fns && st && ai && p && rec.count && nvmtl_air_stores_on()) {
        NSArray *ext = nvmtl_externs_of_airs(ai) ?: @[];
        NSData *out = [NSPropertyListSerialization dataWithPropertyList:@{@"v": @1, @"length": @(lib.length), @"blobs": rec, @"externs": ext}
                                                                 format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
        if (out && nvmtl_store_file(p, out))
            nvlog("library index: disk record stored (%lu blobs, %lu externs) %s", (unsigned long)rec.count, (unsigned long)ext.count, p.lastPathComponent.UTF8String);
        else { nvmtl_cache_store_refused("library index", p); nvlog("library index: could not store the disk record %s", p.lastPathComponent.UTF8String); }
    }
    return fns;
}

typedef struct { char magic[8]; uint64_t dev, ino, size; int64_t mts, mtn, cts, ctn; uint8_t edge[32], key[32]; } nvmtl_lk1_rec;
static _Atomic unsigned long gLkHit, gLkMiss, gLkStale, gLkStored;
NSData *nvmtl_lib_sha256(NSData *lib) {
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    const uint8_t *ptr=lib.bytes; NSUInteger remaining=lib.length;
    while (remaining) { CC_LONG chunk=(CC_LONG)MIN(remaining,(NSUInteger)UINT32_MAX); CC_SHA256_Update(&hash,ptr,chunk);ptr+=chunk;remaining-=chunk; }
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(digest,&hash);
    return [NSData dataWithBytes:digest length:sizeof(digest)];
}
static void nvmtl_lk1_edge(NSData *lib, uint8_t out[32]) {
    const NSUInteger n = lib.length, e = MIN(n, (NSUInteger)65536); const uint8_t *b = lib.bytes; const uint64_t len = n;
    CC_SHA256_CTX h; CC_SHA256_Init(&h); CC_SHA256_Update(&h, &len, sizeof len);
    CC_SHA256_Update(&h, b, (CC_LONG)e); CC_SHA256_Update(&h, b + n - e, (CC_LONG)e); CC_SHA256_Final(out, &h);
}
int nvmtl_stat_same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec && a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
}
NSData *nvmtl_libkey_for_file(NSString *path, NSData *lib, const struct stat *before) {
    if (!path.length || !lib.length || getenv("NVMTL_NO_LIBMEMO")) return nil;
    const int weak = getenv("NVMTL_LIBMEMO_WEAK_TEST") != NULL, noct = weak || getenv("NVMTL_LIBMEMO_NOCTIME_TEST") != NULL;
    struct stat now;
    if (stat(path.fileSystemRepresentation, &now) != 0 || !nvmtl_stat_same(before, &now) || (uint64_t)now.st_size != lib.length) {
        nvlog("library key memo: %s changed while it was read (or cannot be stat'ed) - hashing the bytes, storing nothing", path.UTF8String);
        return nil; }
    uint8_t ph[CC_SHA256_DIGEST_LENGTH]; const char *pp = path.fileSystemRepresentation; CC_SHA256(pp, (CC_LONG)strlen(pp), ph);
    char name[48]; snprintf(name, sizeof name, "lk1-%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.bin",
                            ph[0], ph[1], ph[2], ph[3], ph[4], ph[5], ph[6], ph[7], ph[8], ph[9], ph[10], ph[11]);
    NSString *rp = [nvmtl_aircache_dir() stringByAppendingPathComponent:@(name)];
    uint8_t edge[32]; nvmtl_lk1_edge(lib, edge);
    nvmtl_lk1_rec r; NSData *rd = [NSData dataWithContentsOfFile:rp];
    if (rd.length == sizeof r) {
        memcpy(&r, rd.bytes, sizeof r);
        const int stat_eq = !memcmp(r.magic, "NVLK1\0\0\0", 8) && r.dev == (uint64_t)now.st_dev && r.ino == (uint64_t)now.st_ino &&
            r.size == (uint64_t)now.st_size && r.mts == now.st_mtimespec.tv_sec &&
            (weak || r.mtn == now.st_mtimespec.tv_nsec) && (noct || (r.cts == now.st_ctimespec.tv_sec && r.ctn == now.st_ctimespec.tv_nsec));
        if (stat_eq && weak) {
            nvlog("library key memo: NVMTL_LIBMEMO_WEAK_TEST served the recorded key for %s - no ctime, no content check (negative control only)", path.UTF8String);
            return [NSData dataWithBytes:r.key length:sizeof r.key]; }
        if (stat_eq && !memcmp(r.edge, edge, sizeof edge)) {
            const unsigned long h = atomic_fetch_add(&gLkHit, 1) + 1;
            if (!(h & (h - 1))) nvlog("library key memo: hit %lu (%lu B %s) - whole-library hash skipped; misses %lu stale %lu stored %lu%s",
                                      h, (unsigned long)lib.length, path.lastPathComponent.UTF8String, atomic_load(&gLkMiss),
                                      atomic_load(&gLkStale), atomic_load(&gLkStored), noct ? " (NOCTIME_TEST: ctime ignored)" : "");
            return [NSData dataWithBytes:r.key length:sizeof r.key]; }
        if (stat_eq) {
            atomic_fetch_add(&gLkStale, 1);
            nvlog("library key memo: STALE record refused for %s - stat matches%s but the first/last 64 KiB differ; re-hashing",
                  path.UTF8String, noct ? " (ctime ignored by NOCTIME_TEST)" : "");
        }
    }
    atomic_fetch_add(&gLkMiss, 1);
    NSData *key = nvmtl_lib_sha256(lib);
    memset(&r, 0, sizeof r); memcpy(r.magic, "NVLK1\0\0\0", 8);
    r.dev = (uint64_t)now.st_dev; r.ino = (uint64_t)now.st_ino; r.size = (uint64_t)now.st_size;
    r.mts = now.st_mtimespec.tv_sec; r.mtn = now.st_mtimespec.tv_nsec; r.cts = now.st_ctimespec.tv_sec; r.ctn = now.st_ctimespec.tv_nsec;
    memcpy(r.edge, edge, sizeof edge); memcpy(r.key, key.bytes, sizeof r.key);
    if (!nvmtl_air_stores_on()) return key;
    [[NSFileManager defaultManager] createDirectoryAtPath:nvmtl_aircache_dir() withIntermediateDirectories:YES attributes:nil error:nil];
    if (nvmtl_store_file(rp, [NSData dataWithBytes:&r length:sizeof r])) {
        const unsigned long st = atomic_fetch_add(&gLkStored, 1) + 1;
        if (!(st & (st - 1))) nvlog("library key memo: stored %lu (%lu B %s)", st, (unsigned long)lib.length, path.lastPathComponent.UTF8String);
    } else { nvmtl_cache_store_refused("library key memo", rp); nvlog("library key memo: could not store %s", name); }
    return key;
}
NSDictionary<NSString *, NSData *> *nvmtl_translate_metallib_k(NSData *lib, NSData *knownKey, NSDictionary **stageOut, NSDictionary **airOut);
NSDictionary<NSString *, NSData *> *nvmtl_translate_metallib(NSData *lib, NSDictionary **stageOut, NSDictionary **airOut) {
    return nvmtl_translate_metallib_k(lib, nil, stageOut, airOut); }

// Background translation of a newly indexed library (see NVMTLXlateJob). Entries that are translated only with more
// information are skipped: function-constant users (specialized per constant set), OpenCL entries (__entry_, by-value
// arguments), visible functions, mesh/object stages, and modules that need linking. Up to 128 entries per library
// (a framework's large shader libraries are left to on-demand translation), on at most (cores/2) utility threads.
static NSMutableArray *gBgWork, *gBgWork2;   // blocks, under gXlateCond; 2: function-constant users, after all others
static unsigned gBgWorkers;
static void nvmtl_bg_worker(void) {
    for (;;) {
        dispatch_block_t b = nil;
        [gXlateCond lock];
        if (gBgWork.count) { b = gBgWork.firstObject; [gBgWork removeObjectAtIndex:0]; }
        else if (gBgWork2.count) { b = gBgWork2.firstObject; [gBgWork2 removeObjectAtIndex:0]; }
        else gBgWorkers--;
        [gXlateCond unlock];
        if (!b) return;
        gXlateOnBg = 1;
        @autoreleasepool { b(); }
        gXlateOnBg = 0;
    }
}
static int nvmtl_bg_xlate_on(void) {
    static int on = -1;
    // not in WindowServer by default: its libraries are the system's (CoreImage, MPS: thousands of kernels it never
    // draws with), and translating them ahead only takes CPU at login. NVMTL_BG_XLATE=1 turns it on there.
    if (on < 0) { const char *e = getenv("NVMTL_BG_XLATE"); const BOOL ws = getprogname() && !strcmp(getprogname(), "WindowServer");
        on = e ? e[0] != '0' : !ws;
        nvlog("translate: background translation of new libraries %s (NVMTL_BG_XLATE=%s)", on ? "ON" : "off", e ? e : "unset"); }
    return on;
}
static void nvmtl_bg_translate_library(NSDictionary<NSString *, NSData *> *fns, NSDictionary *stages, NSDictionary *airs) {
    if (!fns.count || !nvmtl_bg_xlate_on() || !nvmtl_xlate_ready() || getenv("NVMTL_SPVCACHE_OFF")) return;
    nvmtl_xlate_jobs_init();
    static unsigned maxw; static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const long n = sysconf(_SC_NPROCESSORS_ONLN); maxw = n > 3 ? (unsigned)(n / 2) : 1;
        q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0); gBgWork = [NSMutableArray new]; gBgWork2 = [NSMutableArray new]; });
    NSUInteger kicked = 0;
    for (NSString *name in fns) {
        if (kicked >= 128) break;
        NSString *stage = stages[name];
        if (![stage isEqualToString:@"vertex"] && ![stage isEqualToString:@"fragment"] && ![stage isEqualToString:@"kernel"]) continue;
        if ([name hasPrefix:@"__entry_"]) continue;
        NSData *air = fns[name];
        if (air.length >= 20 && *(const uint32_t *)air.bytes == 0x07230203u) continue;
        NSString *spvp = nvmtl_spvcache_path(air, stage);
        if (!spvp || access(spvp.fileSystemRepresentation, F_OK) == 0) continue;
        NVMTLXlateJob *job = [NVMTLXlateJob new];
        BOOL spawn = NO;
        [gXlateCond lock];
        if (gXlateJobs[spvp]) { [gXlateCond unlock]; continue; }
        gXlateJobs[spvp] = job;
        __block BOOL second = NO;
        __block dispatch_block_t work = nil;
        work = ^{
            [gXlateCond lock];
            const BOOL mine = gXlateJobs[spvp] == job && job->state == 0;
            if (mine) job->state = 1;
            [gXlateCond unlock];
            if (!mine) { work = nil; return; }   // taken over by a caller, or done
            NSString *ll = airs[name];   // the lazy text is decoded here, off the app's thread
            NSString *why = nil;
            // a function-constant user is translated too (an app that asks for it without constants gets this generic
            // form; MPS and Geekbench do), but only after every other queued entry: its first turn sends it to the back
            if (ll && !second && [ll rangeOfString:@"air.function_constant"].location != NSNotFound) {
                second = YES;
                [gXlateCond lock]; if (gXlateJobs[spvp] == job) job->state = 0; [gBgWork2 addObject:work]; [gXlateCond broadcast]; [gXlateCond unlock];
                return;
            }
            BOOL skip = !ll || [ll rangeOfString:@"@air.dyld_lib_table"].location != NSNotFound
                     || [ll rangeOfString:@"!\"air.intersection_function_table\""].location != NSNotFound;
            if (skip) {   // give the entry back: its caller translates it the way it needs
                // (state 0 out of the table: a caller waiting for it translates it itself)
                [gXlateCond lock]; if (gXlateJobs[spvp] == job) [gXlateJobs removeObjectForKey:spvp]; job->state = 0; [gXlateCond broadcast]; [gXlateCond unlock];
                atomic_fetch_add(&gBgSkipped, 1); work = nil; return;
            }
            // run it as a caller that took the queued job over would (state back to 0 for one claim by this thread)
            [gXlateCond lock]; if (gXlateJobs[spvp] == job) job->state = 0; [gXlateCond unlock];
            (void)nvmtl_compile_library_entry(air, ll, stage, name, &why);
            const unsigned long long d = atomic_fetch_add(&gBgDone, 1) + 1;
            if (d <= 2 || (d & (d - 1)) == 0)
                nvlog("translate (background): %llu of %llu queued done; callers took %llu over, waited for %llu (%.1f ms in all), %llu left to their callers",
                      d, atomic_load(&gBgQueued), atomic_load(&gBgTaken), atomic_load(&gBgWaited), atomic_load(&gBgWaitNs) / 1e6, atomic_load(&gBgSkipped));
            work = nil;   // breaks the block's reference to itself once it has run for good
        };
        [gBgWork addObject:work];
        if (gBgWorkers < maxw) { gBgWorkers++; spawn = YES; }
        [gXlateCond unlock];
        atomic_fetch_add(&gBgQueued, 1); kicked++;
        if (spawn) dispatch_async(q, ^{ nvmtl_bg_worker(); });
    }
}
NSDictionary<NSString *, NSData *> *nvmtl_translate_metallib_k(NSData *lib, NSData *knownKey, NSDictionary **stageOut, NSDictionary **airOut) {
    if (stageOut) *stageOut = nil;
    if (airOut) *airOut = nil;
    if (!lib) return nil;
    static NSObject *lock;
    static NSMutableDictionary *entries;
    static NSMapTable *weakEntries;
    static char metadataKey;
    static NSMutableArray *lru;
    static NSUInteger retainedBytes;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ lock=[NSObject new]; entries=[NSMutableDictionary new]; weakEntries=[NSMapTable strongToWeakObjectsMapTable]; lru=[NSMutableArray new]; });
    NSData *key=knownKey.length==CC_SHA256_DIGEST_LENGTH ? knownKey : nvmtl_lib_sha256(lib);
    @synchronized(lock) {
        NSDictionary *hit=[weakEntries objectForKey:key];
        NSArray *meta=hit ? objc_getAssociatedObject(hit,&metadataKey) : nil;
        if(hit) {
            [lru removeObject:key];[lru addObject:key];
            if(stageOut)*stageOut=meta[0];if(airOut)*airOut=meta[1];
            nvlog("library index: hit bytes=%lu retained=%lu entries=%lu",(unsigned long)lib.length,(unsigned long)retainedBytes,(unsigned long)entries.count);
            return hit;
        }
    }
    NSDictionary *stages=nil,*airs=nil;
    NSDictionary *fns=nvmtl_index_metallib_disk(lib,key,&stages,&airs);
    if(stageOut)*stageOut=stages;if(airOut)*airOut=airs;
    if(!fns || !stages || !airs)return fns;
    const NSUInteger budget=128ull*1024*1024;
    NSUInteger cost=4096;
    for(NSData *data in fns.allValues) { if(cost>budget || data.length>budget-cost){cost=budget+1;break;}cost+=data.length; }
    const BOOL lazyAirs=[airs isKindOfClass:NVMTLLazyAirs.class];
    if(lazyAirs){ NSUInteger units=((NVMTLLazyAirs *)airs).blobBytes*4; if(cost>budget-512 || units>(budget-cost-512)/2) cost=budget+1; else cost+=units*2+512; }
    if(!lazyAirs) for(NSString *name in airs) {
        NSString *ir=airs[name];NSUInteger units=ir.length+name.length;
        if(cost>budget-512 || units>(budget-cost-512)/2){cost=budget+1;break;}
        cost+=units*2+512;
    }
    if (!fns.count) fns = [NSMutableDictionary dictionaryWithDictionary:fns];
    objc_setAssociatedObject(fns,&metadataKey,@[stages,airs,@(cost)],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    @synchronized(lock) {
        NSDictionary *existing=[weakEntries objectForKey:key];
        if(!existing) {
            [lru removeObject:key];
            while(lru.count && (lru.count>=16 || (cost<=budget && retainedBytes>budget-cost))) {
                NSData *old=lru.firstObject;NSDictionary *owned=entries[old];
                if(owned)retainedBytes-=[objc_getAssociatedObject(owned,&metadataKey)[2] unsignedIntegerValue];
                [entries removeObjectForKey:old];[weakEntries removeObjectForKey:old];[lru removeObjectAtIndex:0];
            }
            [weakEntries setObject:fns forKey:key];[lru addObject:key];
            if(cost<=budget){entries[key]=fns;retainedBytes+=cost;}
            nvlog("library index: stored cost=%lu retained=%lu entries=%lu weak=%d",(unsigned long)cost,(unsigned long)retainedBytes,(unsigned long)lru.count,cost>budget);
        }
    }
    nvmtl_bg_translate_library(fns, stages, airs);
    return fns;
}

static NSString *nvmtl_link_dump_dir(void) {
    const char *v = getenv("NVMTL_LINK_DUMP");
    if (!v || !*v) return nil;
    NSString *root = (v[0] == '/') ? [NSString stringWithUTF8String:v] : @"/tmp";
    return root ? [root stringByAppendingFormat:@"/nvmtl-link-%d", getpid()] : nil;
}
// Set by a caller that defers a function needing linked visible functions to pipeline creation. On that path a
// first specialization that stops at an unlinked call is expected, and logging it as a failure put a false
// failure in every report from a Mac drawing the system's icons. Everywhere else the line stays a failure,
// because a caller that is not deferring has no second chance and the refusal is real.
static __thread int g_fc_expect_link;
void nvmtl_fc_expect_link(int on) { g_fc_expect_link = on; }
BOOL nvmtl_needs_visible_link(NSString *air, NSString *why);
// Writes the module handed to the translator (AIR text after linking, stage, constant values, error) to <dir>/fc-<pid>-<n>,
// so a translation can be replayed offline with the same input. Nothing is written when dir is unset.
static void nvmtl_fc_dump(const char *dd, NSString *ll, NSString *stage, const uint32_t *idx, const uint32_t *sizes,
                          const uint8_t *const *payloads, size_t n, const char *e)
{
    if (!dd || !*dd) return;
    NSString *dir = @(dd); static _Atomic unsigned seq;
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *base = [dir stringByAppendingFormat:@"/fc-%d-%u", getpid(), atomic_fetch_add(&seq, 1)];
    [ll writeToFile:[base stringByAppendingString:@".ll"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSMutableString *meta = [NSMutableString stringWithFormat:@"stage %@\nerror %s\n", stage, e ? e : ""];
    for (size_t i = 0; i < n; i++) {
        [meta appendFormat:@"fc %u size %u bytes", idx[i], sizes[i]];
        for (uint32_t b = 0; b < sizes[i] && payloads[i]; b++) [meta appendFormat:@" %02x", payloads[i][b]];
        [meta appendString:@"\n"];
    }
    [meta writeToFile:[base stringByAppendingString:@".txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
NSData *nvmtl_translate_with_fc(NSString *ll, NSString *stage, const uint32_t *idx, const uint32_t *sizes,
                                const uint8_t *const *payloads, size_t n, NSString **err)
{
    if (!nvmtl_xlate_ready()) { if (err) *err = @"the AIR->SPIR-V translator is not loaded"; return nil; }
    if (!xlate_fc) { if (err) *err = @"this translator has no nvmtl_translate_fc — rebuild nvmtl_translate"; return nil; }
    if (!ll || !stage.length) { if (err) *err = @"no AIR kept for this function"; return nil; }
    uint8_t *out = NULL; size_t out_len = 0; char e[512] = {0};
    // NVMTL_FC_DUMP_ALL=<dir>: keep every module before it is translated, including one whose translation never returns.
    nvmtl_fc_dump(getenv("NVMTL_FC_DUMP_ALL"), ll, stage, idx, sizes, payloads, n, NULL);
    if (xlate_fc(ll.UTF8String, stage.UTF8String, idx, sizes, payloads, n, &out, &out_len, e, sizeof e) != 0) {
        if (g_fc_expect_link && nvmtl_needs_visible_link(ll, @(e)))
            nvlog("function constants: this function calls linked visible functions; it is specialized when its pipeline is created");
        else nvlog("function constants: re-translate FAILED: %s", e);
        // NVMTL_FC_FAIL_DUMP=<dir>: keep the refused module (AIR text, stage, constant values, error) so the refusal can be
        // replayed offline against the translator; nothing is written unless the variable is set.
        nvmtl_fc_dump(getenv("NVMTL_FC_FAIL_DUMP"), ll, stage, idx, sizes, payloads, n, e);
        if (err) *err = [NSString stringWithUTF8String:e[0] ? e : "specialization failed"];
        return nil;
    }
    NSData *d = [NSData dataWithBytes:out length:out_len];
    if (xlate_free) xlate_free(out, out_len);
    return d;
}

NSData *nvmtl_translate_with_fc_cached(NSString *ll, NSString *stage, NSString *name, NSDictionary *constants,
                                       const uint32_t *idx, const uint32_t *sizes,
                                       const uint8_t *const *payloads, size_t n, NSString **err)
{
    if (!ll || !constants.count || !nvmtl_xlate_ready())
        return nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err);
    NSDictionary *config = @{ @"stage": stage ?: @"", @"entry": name ?: @"", @"constants": constants };
    NSData *json = [NSJSONSerialization dataWithJSONObject:config options:NSJSONWritingSortedKeys error:NULL];
    NVMTLAirDigest *airDigest = nvmtl_air_state(ll);
    if (!json || !airDigest) return nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err);
    NSString *path = nvmtl_spvcache_path_air(airDigest, json, @"fc1");
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    const BOOL fcCacheOn = path && !getenv("NVMTL_FCCACHE_OFF");   // no folder, no key: NSCache throws on a nil key
    NSData *hit = fcCacheOn ? [nvmtl_memo() objectForKey:path] : nil;
    if (hit) { gFcHit++; utimes(path.fileSystemRepresentation, NULL); return hit; }   // keep the trimmer's age order
    if (fcCacheOn) hit = nvmtl_cache_read(path);
    uint32_t magic = 0;
    if (hit.length >= 20 && hit.length % 4 == 0) {
        memcpy(&magic, hit.bytes, 4);
        if (magic == 0x07230203) {
            gFcHit++; utimes(path.fileSystemRepresentation, NULL);
            [nvmtl_memo() setObject:hit forKey:path cost:hit.length];
            nvlog("fc cache: HIT %s %lu B %.1f ms [%lu hit, %lu miss, %lu stored]", name.UTF8String,
                  (unsigned long)hit.length, (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6,
                  gFcHit, gFcMiss, gFcStored);
            return hit;
        }
    }
    gFcMiss++;
    if (nvmtl_hitch_ms()) nvmtl_hitch_add_ns(NVH_SPVMISS, 0);
    NSData *result = nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err);
    if (result) {
        if (path && nvmtl_cache_store(path, result)) gFcStored++;
        if (fcCacheOn && nvmtl_is_spirv(result)) [nvmtl_memo() setObject:result forKey:path cost:result.length];
        nvlog("fc cache: miss %s %lu B %.1f ms [%lu hit, %lu miss, %lu stored]", name.UTF8String,
              (unsigned long)result.length, (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6,
              gFcHit, gFcMiss, gFcStored);
    }
    return result;
}

/* Function constants as specialization constants (nvmtl_spec_fc.h). A function asked with constant values used to be
 * translated whole for each set of values: CoreAnimation's uber shaders (fixed_frag_lph_cph, 600 KB of SPIR-V) took
 * 1.3-2.5 s each time, on WindowServer's thread, so every new variant froze the desktop once. All of that time is the
 * translator's (the AIR-level constant substitution is 5 ms; the rest is emission, inlining and CFG work).
 *
 * The constants a function's shape depends on - those the static initializer folds into [[function_constant]]
 * predicates, i.e. which arguments, inputs and outputs exist - and every boolean constant (nvmtl_fc_structural;
 * fixed_frag_lph_cph: 39 of 59) are baked in as before, and every supplied
 * constant is defined (is_function_constant_defined true: else `select defined, value, 0` folded to 0 before the
 * value could matter - the first version's every variant rendered as all-zero constants). Every other constant stays
 * unknown while the function is translated (nothing derived from it is folded) and becomes a specialization constant
 * (nvmtl_translate_fc_spec2). Checked offline: a variant frozen from this module is, instruction for instruction with
 * constants resolved, the module translated with all the values baked (fixed_frag_lph_cph, fcvar variants 0-5). That module is kept per set of structural values (disk spvcache, tag fcspec3, and
 * memory), and a variant is that module with all its values written in (frozen into plain constants by default:
 * NVMTL_FC_SPEC_FREEZE=0 keeps them specialization constants with these defaults) - a pass over the words. ANV folds
 * them when it compiles. The interface depends on the structural values only, so a variant's own reflection describes
 * the module. A module the translator cannot carry this way keeps the old path (the refusal is remembered).
 *
 * NVMTL_FC_SPEC=0: off. Default: fragment functions. NVMTL_FC_SPEC=all: vertex and kernel functions too. */
int nvmtl_fc_spec_wanted(NSString *stage) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("NVMTL_FC_SPEC"); mode = !e || !*e ? 1 : !strcmp(e, "0") ? 0 : !strcmp(e, "all") ? 2 : 1; }
    if (!mode || !stage || !xlate_fc_spec2 || !xlate_fc_structural) return 0;
    if ([stage isEqualToString:@"fragment"]) return 1;
    return mode == 2 && ([stage isEqualToString:@"vertex"] || [stage isEqualToString:@"kernel"]);
}
static _Atomic unsigned long gSpecHit, gSpecMiss, gSpecRefused;
// the structural constants of a module, by its AIR digest
static NSIndexSet *nvmtl_fc_structural_of(NSString *ll, NSString *digestHex)
{
    static NSMutableDictionary<NSString *, NSIndexSet *> *memo; static dispatch_once_t once;
    dispatch_once(&once, ^{ memo = [NSMutableDictionary new]; });
    @synchronized (memo) { NSIndexSet *hit = memo[digestHex]; if (hit) return hit; }
    uint8_t *out = NULL; size_t len = 0; char e[256] = { 0 };
    if (xlate_fc_structural(ll.UTF8String, &out, &len, e, sizeof e) != 0) return nil;
    NSArray *a = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:out length:len] options:0 error:NULL];
    if (xlate_free) xlate_free(out, len);
    if (![a isKindOfClass:[NSArray class]]) return nil;
    NSMutableIndexSet *set = [NSMutableIndexSet indexSet];
    for (id x in a) if ([x isKindOfClass:[NSNumber class]]) [set addIndex:[x unsignedIntegerValue]];
    @synchronized (memo) { memo[digestHex] = set; if (memo.count > 512) [memo removeAllObjects]; }
    return set;
}
// The module of the function for the structural values among (idx, sizes, payloads), translated once per such set.
NSData *nvmtl_translate_fc_spec_cached(NSString *ll, NSString *stage, NSString *name, const uint32_t *idx,
                                       const uint32_t *sizes, const uint8_t *const *payloads, size_t n, NSString **err)
{
    if (err) *err = nil;
    if (!ll || !stage.length || !nvmtl_xlate_ready() || !xlate_fc_spec2 || !xlate_fc_structural) return nil;
    NVMTLAirDigest *airDigest = nvmtl_air_state(ll);
    if (!airDigest) return nil;
    unsigned char h[CC_SHA256_DIGEST_LENGTH]; { CC_SHA256_CTX c = airDigest->_ctx; CC_SHA256_Final(h, &c); }
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    NSIndexSet *structural = nvmtl_fc_structural_of(ll, @(hex));
    if (!structural) { if (err) *err = @"structural analysis failed"; return nil; }
    uint32_t sidx[256], ssz[256]; const uint8_t *spay[256]; size_t sn = 0;
    NSMutableDictionary *baked = [NSMutableDictionary new];
    for (size_t k = 0; k < n && sn < 256; k++) {
        if (![structural containsIndex:idx[k]]) continue;
        sidx[sn] = idx[k]; ssz[sn] = sizes[k]; spay[sn] = payloads[k]; sn++;
        NSMutableArray *bytes = [NSMutableArray new]; for (uint32_t j = 0; j < sizes[k]; j++) [bytes addObject:@(payloads[k][j])];
        baked[[@(idx[k]) stringValue]] = bytes;
    }
    // every supplied constant is defined (is_function_constant_defined), its value baked or specialized: the set is
    // part of the module
    NSMutableArray *defined = [NSMutableArray new]; for (size_t k = 0; k < n; k++) [defined addObject:@(idx[k])];
    [defined sortUsingSelector:@selector(compare:)];
    NSDictionary *config = @{ @"stage": stage, @"entry": name ?: @"", @"spec": @5, @"structural": baked, @"defined": defined };
    NSData *json = [NSJSONSerialization dataWithJSONObject:config options:NSJSONWritingSortedKeys error:NULL];
    NSString *path = json ? nvmtl_spvcache_path_air(airDigest, json, @"fcspec5") : nil;
    const BOOL cacheOn = path && !getenv("NVMTL_FCCACHE_OFF");
    // a module the translator refused is not tried again in this process (it costs a whole translation each time),
    // nor in the next one while its refusal is on disk
    static NSMutableSet *refused; static NSMutableDictionary *locks; static dispatch_once_t once;
    dispatch_once(&once, ^{ refused = [NSMutableSet new]; locks = [NSMutableDictionary new]; });
    // No cache folder (a sandbox, e.g. WebKit's GPU process): the key has to tell the structural values apart by itself.
    // %@ of an NSData prints only its length and a few of its bytes, so variants whose JSON has the same length shared
    // one module: rounded boxes and gradients drew through the module of a plain fill.
    NSString *jsonText = json ? [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] : nil;
    NSString *key = path ?: [NSString stringWithFormat:@"%s|%@|%@|%@", hex, stage, name, jsonText];
    id lock;
    @synchronized (locks) {
        if ([refused containsObject:key]) { if (err) *err = @"refused earlier in this process"; return nil; }
        lock = locks[key]; if (!lock) { lock = [NSObject new]; locks[key] = lock; }
    }
    @synchronized (lock) {   // one translation per module at a time; the others wait for it and hit
        // the memory copy is kept even with NVMTL_FCCACHE_OFF: the module serves every variant with these structural values
        NSData *hit = [nvmtl_memo() objectForKey:key];
        if (hit) { gSpecHit++; return hit; }
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        NSString *refusedPath = [path stringByAppendingString:@".refused"];
        if (cacheOn && access(refusedPath.fileSystemRepresentation, F_OK) == 0) {
            @synchronized (locks) { [refused addObject:key]; }
            if (err) *err = @"refused (on disk)"; return nil;
        }
        if (cacheOn) hit = nvmtl_cache_read(path);
        if (nvmtl_is_spirv(hit)) {
            gSpecHit++; utimes(path.fileSystemRepresentation, NULL);
            [nvmtl_memo() setObject:hit forKey:key cost:hit.length];
            nvlog("fc spec: HIT %s %lu B %.1f ms [%lu hit, %lu miss, %lu refused]", name.UTF8String, (unsigned long)hit.length,
                  (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6, gSpecHit, gSpecMiss, gSpecRefused);
            return hit;
        }
        if (nvmtl_hitch_ms()) nvmtl_hitch_add_ns(NVH_SPVMISS, 0);
        uint8_t *out = NULL; size_t out_len = 0; uint32_t count = 0; char e[512] = { 0 };
        const int rc = xlate_fc_spec2(ll.UTF8String, stage.UTF8String, sidx, ssz, spay, sn, idx, n, &count, &out, &out_len, e, sizeof e);
        const double ms = (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6;
        if (rc != 0 || !count) {
            if (out && xlate_free) xlate_free(out, out_len);
            gSpecRefused++;
            @synchronized (locks) { [refused addObject:key]; }
            NSString *why = rc ? @(e[0] ? e : "translation failed") : @"the function has no function constants left to specialize";
            if (cacheOn) nvmtl_store_text(refusedPath, why);
            nvlog("fc spec: %s not carried as specialization constants (%.1f ms): %s - its variants are translated whole",
                  name.UTF8String, ms, why.UTF8String);
            if (err) *err = why;
            return nil;
        }
        NSData *d = [NSData dataWithBytes:out length:out_len];
        if (xlate_free) xlate_free(out, out_len);
        gSpecMiss++;
        if (cacheOn) nvmtl_cache_store(path, d);
        [nvmtl_memo() setObject:d forKey:key cost:d.length];
        nvlog("fc spec: miss %s %lu B, %zu structural constant(s) baked, %u specialization constants, %.1f ms [%lu hit, %lu miss, %lu refused]",
              name.UTF8String, (unsigned long)d.length, sn, count, ms, gSpecHit, gSpecMiss, gSpecRefused);
        return d;
    }
}
// The module of one variant: `spec` with the values written in (nvmtl_spec_fc.h).
NSData *nvmtl_fc_spec_variant(NSData *spec, const uint32_t *idx, const uint32_t *sizes, const uint8_t *const *payloads, size_t n)
{
    if (!nvmtl_is_spirv(spec)) return nil;
    size_t words = 0; unsigned applied = 0;
    // NVMTL_FC_SPEC_FREEZE=0: the values stay specialization constants (their defaults); otherwise they become plain
    // constants, the module of the variant then being what a translation with the values would declare
    static int freeze = -1;
    if (freeze < 0) { const char *e = getenv("NVMTL_FC_SPEC_FREEZE"); freeze = !(e && e[0] == '0'); }
    uint32_t *w = nvmtl_spec_fc_apply_ex(spec.bytes, spec.length / 4, idx, sizes, payloads, n, &words, &applied, freeze);
    if (!w) return nil;
    return [NSData dataWithBytesNoCopy:w length:words * 4 freeWhenDone:YES];
}

static int nvmtl_reflect_air(NVMTLFunction *function, NSString *ll, NSString *stage,
                             uint8_t **j, size_t *jn, char *e, size_t elen)
{
    NSDictionary *fc = function ? function->_fc : nil;
    BOOL have = [fc isKindOfClass:[NSDictionary class]] && fc.count > 0;
    if (have && xlate_refl_fc) {
        NSUInteger capacity = fc.count;
        __attribute__((objc_precise_lifetime)) NSMutableData *packed = nvmtl_fc_storage(capacity);
        uint32_t *idx = packed.mutableBytes, *sz = idx ? idx + capacity : NULL;
        const uint8_t **pay = sz ? (const uint8_t **)(sz + capacity) : NULL;
        uint8_t (*bytes)[16] = pay ? (uint8_t (*)[16])(pay + capacity) : NULL; size_t n = 0, dropped = 0;
        if (!packed) { snprintf(e, elen, "function constant reflection allocation failed"); return -1; }
        for (NSString *k in fc) {
            NSArray *a = fc[k];
            if (![k isKindOfClass:[NSString class]] ||
                ![a isKindOfClass:[NSArray class]] || a.count == 0 || a.count > 16) { dropped++; continue; }
            idx[n] = (uint32_t)k.integerValue; sz[n] = (uint32_t)a.count;
            for (NSUInteger m = 0; m < a.count; m++) bytes[n][m] = (uint8_t)[a[m] unsignedCharValue];
            pay[n] = bytes[n]; n++;
        }
        if (dropped) { snprintf(e, elen, "function constant reflection refused %lu malformed value(s)", (unsigned long)dropped); return -1; }
        if (n) {
            int rc = xlate_refl_fc(ll.UTF8String, stage.UTF8String, idx, sz, pay, n, j, jn, e, elen);
            nvlog("reflection: %s carried %zu function constant(s) into reflection (rc %d)",
                  function->_fname.UTF8String ?: "?", n, rc);
            return rc;
        }
    }
    if (have && !xlate_refl_fc)
        nvlog("reflection: %s carries %lu function constant(s) but this translator exports no "
              "nvmtl_reflect_fc - any binding slot derived from a function constant WILL reflect at 0",
              function->_fname.UTF8String ?: "?", (unsigned long)fc.count);
    return xlate_refl(ll.UTF8String, stage.UTF8String, j, jn, e, elen);
}

static _Atomic unsigned long gRfHit, gRfMiss, gRfStored;
static NSData *nvmtl_reflect_air_cached(NVMTLFunction *function, NSString *ll, NSString *stage, char *e, size_t elen)
{
    NVMTLAirDigest *airDigest = nvmtl_air_state(ll);
    NSDictionary *fc = (function && [function->_fc isKindOfClass:[NSDictionary class]]) ? function->_fc : @{};
    NSDictionary *config = @{ @"stage": stage ?: @"", @"fc": fc, @"fcx": @(xlate_refl_fc != NULL) };
    NSData *json = [NSJSONSerialization isValidJSONObject:config]
                 ? [NSJSONSerialization dataWithJSONObject:config options:NSJSONWritingSortedKeys error:NULL] : nil;
    NSString *path = (airDigest && json) ? nvmtl_spvcache_path_air(airDigest, json, @"refl1") : nil;
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    const BOOL useCache = path && !getenv("NVMTL_REFLCACHE_OFF") && !getenv("NVMTL_SPVCACHE_OFF");
    if (useCache) {
        NSData *mem = [nvmtl_memo() objectForKey:path];
        if (mem) { ++gRfHit; utimes(path.fileSystemRepresentation, NULL); return mem; }
        NSData *hit = nvmtl_cache_read(path);
        id obj = hit.length ? [NSJSONSerialization JSONObjectWithData:hit options:0 error:NULL] : nil;
        if ([obj isKindOfClass:[NSDictionary class]] && [(NSDictionary *)obj count]) {
            unsigned long hn = ++gRfHit; utimes(path.fileSystemRepresentation, NULL);
            [nvmtl_memo() setObject:hit forKey:path cost:hit.length];
            if (!(hn & (hn - 1))) nvlog("refl cache: HIT %s %lu B %.2f ms [%lu hit, %lu miss, %lu stored]", function ? function->_fname.UTF8String : "?",
                                        (unsigned long)hit.length, (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6, hn, (unsigned long)gRfMiss, (unsigned long)gRfStored);
            return hit;
        }
    }
    unsigned long mn = ++gRfMiss;
    uint8_t *j = NULL; size_t jn = 0;
    if (nvmtl_reflect_air(function, ll, stage, &j, &jn, e, elen) != 0) return nil;
    NSData *d = [NSData dataWithBytes:j length:jn];
    if (xlate_free) xlate_free(j, jn);
    id obj = d.length ? [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL] : nil;
    if (path && [obj isKindOfClass:[NSDictionary class]] && [(NSDictionary *)obj count] && nvmtl_cache_store(path, d)) gRfStored++;
    if (useCache && [obj isKindOfClass:[NSDictionary class]] && [(NSDictionary *)obj count]) [nvmtl_memo() setObject:d forKey:path cost:d.length];
    if (!(mn & (mn - 1))) nvlog("refl cache: miss %s %zu B %.2f ms [%lu hit, %lu miss, %lu stored]", function ? function->_fname.UTF8String : "?",
                                jn, (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6, (unsigned long)gRfHit, mn, (unsigned long)gRfStored);
    return d;
}

static _Atomic unsigned long gLkHit, gLkMiss, gLkStored;
static NSData *nvmtl_translate_link_cached(NSString *ll, NSString *stage, const uint32_t *idx, const uint32_t *sizes,
                                           const uint8_t *const *payloads, size_t n, NSString **err)
{
    NSData *air = [ll dataUsingEncoding:NSUTF8StringEncoding];
    NSData *st = [(stage ?: @"") dataUsingEncoding:NSUTF8StringEncoding];
    size_t *ord = calloc(n ? n : 1, sizeof *ord);
    if (!air || !st || !ord || !nvmtl_xlate_ready()) { free(ord); return nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err); }
    for (size_t k = 0; k < n; k++) ord[k] = k;
    for (size_t a = 1; a < n; a++) for (size_t b = a; b > 0 && idx[ord[b - 1]] > idx[ord[b]]; b--) { size_t x = ord[b]; ord[b] = ord[b - 1]; ord[b - 1] = x; }
    NSMutableData *key = [NSMutableData data]; uint64_t size = air.length, sl = st.length, nn = n;
    [key appendBytes:&size length:sizeof size]; [key appendData:air];
    [key appendBytes:&sl length:sizeof sl]; [key appendData:st]; [key appendBytes:&nn length:sizeof nn];
    for (size_t k = 0; k < n; k++) { size_t q = ord[k];
        [key appendBytes:&idx[q] length:sizeof idx[q]]; [key appendBytes:&sizes[q] length:sizeof sizes[q]];
        if (sizes[q] && payloads[q]) [key appendBytes:payloads[q] length:sizes[q]]; }
    free(ord);
    NSString *path = nvmtl_spvcache_path(key, @"lk1");
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    NSData *hit = (getenv("NVMTL_LINKCACHE_OFF") || getenv("NVMTL_FCCACHE_OFF")) ? nil : nvmtl_cache_read(path);
    uint32_t magic = 0;
    if (hit.length >= 20 && hit.length % 4 == 0) {
        memcpy(&magic, hit.bytes, 4);
        if (magic == 0x07230203) {
            unsigned long hn = ++gLkHit; utimes(path.fileSystemRepresentation, NULL);
            if (!(hn & (hn - 1))) nvlog("link cache: HIT %lu B %.2f ms [%lu hit, %lu miss, %lu stored]", (unsigned long)hit.length,
                                        (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6, hn, (unsigned long)gLkMiss, (unsigned long)gLkStored);
            return hit;
        }
    }
    unsigned long mn = ++gLkMiss;
    NSData *result = nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err);
    if (result && path && nvmtl_cache_store(path, result)) gLkStored++;
    if (!(mn & (mn - 1))) nvlog("link cache: miss %lu B %.2f ms [%lu hit, %lu miss, %lu stored]", (unsigned long)result.length,
                                (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6, (unsigned long)gLkHit, mn, (unsigned long)gLkStored);
    return result;
}

NSDictionary *nvmtl_function_reflection(NVMTLFunction *function)
{
    if (!function) return nil;
    @synchronized (function) {
        if (function->_refl) return function->_refl.count ? function->_refl : nil;
        function->_refl = @{};
        if (!nvmtl_xlate_ready() || !xlate_refl) { nvlog("reflection: translator not ready — %s cannot be reflected", function->_fname.UTF8String ?: "?"); return nil; }
        NSString *ll = function->_air, *stage = function->_stage, *name = function->_fname;
        if (!ll || !stage.length) { nvlog("reflection: %s has no AIR — cannot be reflected", name.UTF8String ?: "?"); return nil; }
        char e[512] = { 0 };
        NSData *d = nvmtl_reflect_air_cached(function, ll, stage, e, sizeof e);
        if (!d) { nvlog("reflection for %s FAILED: %s", name.UTF8String, e); return nil; }
        size_t jn = d.length;
        NSDictionary *refl = [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL];
        if ([refl isKindOfClass:[NSDictionary class]] && refl.count) function->_refl = refl;
        else nvlog("reflection for %s: JSON did not parse (%zu bytes)", name.UTF8String, jn);
        return function->_refl.count ? function->_refl : nil;
    }
}

static NSString *nvmtl_air_quoted_after(NSString *line, NSString *key, NSUInteger from) {
    NSString *k = [NSString stringWithFormat:@"!\"%@\", !\"", key];
    NSRange r = [line rangeOfString:k options:0 range:NSMakeRange(from, line.length - from)];
    if (r.location == NSNotFound) return nil;
    NSUInteger s = r.location + r.length;
    NSRange q = [line rangeOfString:@"\"" options:0 range:NSMakeRange(s, line.length - s)];
    return q.location == NSNotFound ? nil : [line substringWithRange:NSMakeRange(s, q.location - s)];
}
static NSInteger nvmtl_air_i32_after(NSString *line, NSString *key) {
    NSString *k = [NSString stringWithFormat:@"!\"%@\", i32 ", key];
    NSRange r = [line rangeOfString:k];
    if (r.location == NSNotFound) return -1;
    return [[line substringFromIndex:r.location + r.length] integerValue];
}
static NSString *nvmtl_air_node_after(NSString *line, NSString *key, NSUInteger from, NSUInteger *end) {
    NSString *k = [NSString stringWithFormat:@"!\"%@\", !", key];
    NSRange r = [line rangeOfString:k options:0 range:NSMakeRange(from, line.length - from)];
    if (r.location == NSNotFound) return nil;
    NSUInteger s = r.location + r.length, e = s;
    while (e < line.length && isdigit([line characterAtIndex:e])) e++;
    if (end) *end = e;
    return e > s ? [@"!" stringByAppendingString:[line substringWithRange:NSMakeRange(s, e - s)]] : nil;
}
static NSString *nvmtl_air_quoted_before(NSString *line, NSUInteger at, NSUInteger *start) {
    if (at < 4 || [line characterAtIndex:at - 1] != '"') return nil;
    NSRange q = [line rangeOfString:@"!\"" options:NSBackwardsSearch range:NSMakeRange(0, at - 1)];
    if (q.location == NSNotFound) return nil;
    if (start) *start = q.location;
    return [line substringWithRange:NSMakeRange(q.location + 2, at - 1 - (q.location + 2))];
}
static void nvmtl_air_walk_struct(NSDictionary *nodes, NSString *node, NSString *bufKey, NSMutableDictionary *members, NSMutableDictionary *types, NSMutableSet *seen) {
    if (!node || [seen containsObject:node]) return;
    [seen addObject:node];
    NSString *line = nodes[node]; if (!line) return;
    NSString *tag = @"!\"air.indirect_argument\", ";
    NSRange r = [line rangeOfString:tag];
    while (r.location != NSNotFound) {
        NSUInteger nameStart = 0; NSString *name = r.location >= 2 ? nvmtl_air_quoted_before(line, r.location - 2, &nameStart) : nil;
        NSString *type = (name && nameStart >= 2) ? nvmtl_air_quoted_before(line, nameStart - 2, NULL) : nil;
        NSUInteger after = r.location + r.length;
        NSInteger id_ = -1;
        if ([line rangeOfString:@"i32 " options:NSAnchoredSearch range:NSMakeRange(after, line.length - after)].location != NSNotFound) {
            id_ = [[line substringFromIndex:after + 4] integerValue];
        } else if (after < line.length && [line characterAtIndex:after] == '!') {
            NSUInteger e = after + 1; while (e < line.length && isdigit([line characterAtIndex:e])) e++;
            NSString *ref = [line substringWithRange:NSMakeRange(after, e - after)];
            NSString *argLine = nodes[ref];
            if (argLine) {
                id_ = nvmtl_air_i32_after(argLine, @"air.location_index");
                if (id_ < 0) id_ = [[argLine substringFromIndex:MIN(argLine.length, (NSUInteger)[argLine rangeOfString:@"!{i32 "].location + 6)] integerValue];
            }
        }
        if (name && id_ >= 0) {
            NSString *k = [NSString stringWithFormat:@"%@:%ld", bufKey, (long)id_];
            if (!members[k]) { members[k] = name; if (type) types[k] = type; }
        }
        r = [line rangeOfString:tag options:0 range:NSMakeRange(after, line.length - after)];
    }
}
static void nvmtl_air_plain_struct(NSDictionary *nodes, NSString *node, NSInteger loc, NSMutableDictionary *plain) {
    NSString *line = node ? nodes[node] : nil; if (!line || loc < 0) return;
    if ([line rangeOfString:@"!\"air.indirect_argument\""].location != NSNotFound) return;
    NSRange br = [line rangeOfString:@"!{"]; if (br.location == NSNotFound) return;
    NSMutableArray *tok = [NSMutableArray array]; NSMutableString *cur = [NSMutableString string]; BOOL q = NO;
    for (NSUInteger i = br.location + 2; i < line.length; i++) {
        unichar c = [line characterAtIndex:i];
        if (c == '"') q = !q;
        if (!q && (c == ',' || c == '}')) { NSString *t = [cur stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]; if (t.length) [tok addObject:t]; [cur setString:@""]; if (c == '}') break; continue; }
        [cur appendFormat:@"%C", c];
    }
    NSMutableArray *ms = [NSMutableArray array];
    for (NSUInteger i = 0; i + 4 < tok.count; ) {
        NSString *a = tok[i], *b = tok[i + 1], *c = tok[i + 2], *t = tok[i + 3], *n = tok[i + 4];
        if ([a hasPrefix:@"i32 "] && [b hasPrefix:@"i32 "] && [c hasPrefix:@"i32 "] && [t hasPrefix:@"!\""] && [n hasPrefix:@"!\""] && t.length > 3 && n.length > 3) {
            [ms addObject:@{ @"off": @([[a substringFromIndex:4] integerValue]), @"size": @([[b substringFromIndex:4] integerValue]),
                             @"count": @([[c substringFromIndex:4] integerValue]),
                             @"type": [t substringWithRange:NSMakeRange(2, t.length - 3)], @"name": [n substringWithRange:NSMakeRange(2, n.length - 3)] }];
            i += 5;
        } else i++;
    }
    if (ms.count) plain[@(loc)] = ms;
}
NSDictionary *nvmtl_air_names(NVMTLFunction *fn)
{
    if (!fn) return nil;
    @synchronized (fn) {
        if (fn->_airNames) return fn->_airNames;
        NSMutableDictionary *args = [NSMutableDictionary new], *members = [NSMutableDictionary new], *types = [NSMutableDictionary new];
        NSMutableDictionary *plain = [NSMutableDictionary new];
        // for pipeline reflection: layout "buffer:N" / "threadgroup:N" -> @[air.arg_type_size, air.arg_type_align_size];
        // access "buffer:N" / "texture:N" -> the declared MTLBindingAccess (air.read / air.write / air.read_write; a
        // sampled texture reads), which is what Metal reports, not what the shader happens to do with it
        NSMutableDictionary *layout = [NSMutableDictionary new], *access = [NSMutableDictionary new];
        fn->_airNames = @{ @"args": args, @"members": members, @"types": types, @"plain": plain, @"layout": layout, @"access": access };
        NSString *ll = fn->_air, *fname = fn->_fname;
        if (!ll.length || !fname.length) return fn->_airNames;
        NSMutableDictionary *nodes = [NSMutableDictionary new];
        NSString *entryLine = nil; NSString *entryNeedle = [NSString stringWithFormat:@"@%@, ", fname];
        NSArray *lines = [ll componentsSeparatedByString:@"\n"];
        for (NSString *line in lines) {
            if (![line hasPrefix:@"!"]) continue;
            NSRange eq = [line rangeOfString:@" = !{"]; if (eq.location == NSNotFound) continue;
            nodes[[line substringToIndex:eq.location]] = line;
            if (!entryLine && [line rangeOfString:entryNeedle].location != NSNotFound) entryLine = line;
        }
        if (!entryLine) { nvlog("air names: no entry node for %s", fname.UTF8String); return fn->_airNames; }
        NSRange nr = [entryLine rangeOfString:entryNeedle];
        NSArray *erefs = [[entryLine substringFromIndex:nr.location + nr.length] componentsSeparatedByString:@", "];
        if (erefs.count < 2) { nvlog("air names: the entry node for %s has %lu reference(s) after the function, no argument list — "
            "nothing named", fname.UTF8String, (unsigned long)erefs.count); return fn->_airNames; }
        NSString *argsRef = [erefs[1] stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"} "]];
        NSString *argsLine = nodes[argsRef]; if (!argsLine) return fn->_airNames;
        NSRange br = [argsLine rangeOfString:@"!{"]; if (br.location == NSNotFound) return fn->_airNames;
        NSArray *refs = [[[argsLine substringFromIndex:br.location + 2] stringByReplacingOccurrencesOfString:@"}" withString:@""] componentsSeparatedByString:@", "];
        for (NSString *ref0 in refs) {
            NSString *ref = [ref0 stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            NSString *line = nodes[ref]; if (!line) continue;
            NSString *kind = nil;
            if ([line rangeOfString:@"!\"air.buffer\""].location != NSNotFound || [line rangeOfString:@"!\"air.indirect_buffer\""].location != NSNotFound) kind = @"buffer";
            else if ([line rangeOfString:@"!\"air.texture\""].location != NSNotFound) kind = @"texture";
            else if ([line rangeOfString:@"!\"air.sampler\""].location != NSNotFound) kind = @"sampler";
            else continue;
            // a threadgroup pointer ([[threadgroup(N)]]) is an air.buffer in address space 3 with an index space of its
            // own: named apart, or it took the name of [[buffer(N)]]
            if ([kind isEqualToString:@"buffer"] && nvmtl_air_i32_after(line, @"air.address_space") == 3) kind = @"threadgroup";
            NSInteger loc = nvmtl_air_i32_after(line, @"air.location_index");
            NSString *name = nvmtl_air_quoted_after(line, @"air.arg_name", 0);
            if (loc < 0 || !name) continue;
            NSString *k = [NSString stringWithFormat:@"%@:%ld", kind, (long)loc];
            args[k] = name;
            NSString *tn = nvmtl_air_quoted_after(line, @"air.arg_type_name", 0); if (tn) types[k] = tn;
            { const NSInteger sz = nvmtl_air_i32_after(line, @"air.arg_type_size"), al = nvmtl_air_i32_after(line, @"air.arg_type_align_size");
              if (sz > 0 && al > 0) layout[k] = @[@(sz), @(al)]; }
            if ([line rangeOfString:@"!\"air.read_write\""].location != NSNotFound) access[k] = @(MTLBindingAccessReadWrite);
            else if ([line rangeOfString:@"!\"air.write\""].location != NSNotFound) access[k] = @(MTLBindingAccessWriteOnly);
            else if ([line rangeOfString:@"!\"air.read\""].location != NSNotFound || [line rangeOfString:@"!\"air.sample\""].location != NSNotFound)
                access[k] = @(MTLBindingAccessReadOnly);
            if ([kind isEqualToString:@"buffer"]) {
                NSString *st = nvmtl_air_node_after(line, @"air.struct_type_info", 0, NULL);
                if (st) nvmtl_air_walk_struct(nodes, st, [NSString stringWithFormat:@"%ld", (long)loc], members, types, [NSMutableSet new]);
                if (st) nvmtl_air_plain_struct(nodes, st, loc, plain);
            }
        }
        NSUInteger np = 0; for (NSArray *pm in plain.allValues) np += pm.count;
        nvlog("air names: %s — %lu argument(s), %lu struct member(s) named, %lu plain-struct member(s)", fname.UTF8String, (unsigned long)args.count, (unsigned long)members.count, (unsigned long)np);
        return fn->_airNames;
    }
}

NSUInteger nvmtl_air_max_tg(NVMTLFunction *fn)
{
    NSString *ll = fn ? fn->_air : nil, *fname = fn ? fn->_fname : nil;
    if (!ll.length || !fname.length) return 0;
    for (NSString *needle in @[[NSString stringWithFormat:@"@%@, ", fname], [NSString stringWithFormat:@"@\"%@\", ", fname]])
    for (NSRange r = NSMakeRange(0, ll.length);;) {
        NSRange m = [ll rangeOfString:needle options:NSLiteralSearch range:r];
        if (m.location == NSNotFound) break;
        NSRange ln = [ll lineRangeForRange:m];
        r = NSMakeRange(NSMaxRange(ln), ll.length - NSMaxRange(ln));
        if ([ll characterAtIndex:ln.location] != '!' || [ll rangeOfString:@" = !{" options:NSLiteralSearch range:ln].location == NSNotFound) continue;
        NSString *tail = [ll substringWithRange:NSMakeRange(NSMaxRange(m), NSMaxRange(ln) - NSMaxRange(m))];
        NSArray *refs = [[tail stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"}\r\n "]] componentsSeparatedByString:@", "];
        for (NSUInteger k = 2; k < refs.count; k++) {
            NSString *want = [NSString stringWithFormat:@"\n%@ = !{!\"air.max_work_group_size\", i32 ", refs[k]];
            NSRange a = [ll rangeOfString:want options:NSLiteralSearch];
            if (a.location != NSNotFound)
                return (NSUInteger)MAX(0, [[ll substringWithRange:NSMakeRange(NSMaxRange(a), MIN((NSUInteger)12, ll.length - NSMaxRange(a)))] integerValue]);
        }
        return 0;
    }
    return 0;
}

// The size and ABI alignment of one LLVM type written in AIR text, as AIR's data layout lays it out (pointers 8 bytes,
// vectors aligned to their power-of-two size up to 128 bytes, structs naturally padded unless packed). Returns the end
// of the type, or NULL for a type it does not know (an opaque struct, a function type).
typedef struct { unsigned long long size, align; } nvmtl_ll_layout;
static unsigned long long nvmtl_ll_pow2(unsigned long long v) { unsigned long long a = 1; while (a < v) a <<= 1; return a; }
static unsigned long long nvmtl_ll_round(unsigned long long v, unsigned long long a) { return a > 1 ? (v + a - 1) / a * a : v; }
static const char *nvmtl_ll_ws(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
static const char *nvmtl_ll_word(const char *p, const char *w) {
    size_t n = strlen(w);
    if (strncmp(p, w, n) || isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '.') return NULL;
    return p + n;
}
static const char *nvmtl_ll_type(const char *p, NSDictionary<NSString *, NSString *> *named, int depth, nvmtl_ll_layout *out)
{
    if (depth > 32) return NULL;
    p = nvmtl_ll_ws(p); const char *q;
    if (*p == '[' || (*p == '<' && p[1] != '{')) {   // [N x T] or <N x T>
        const BOOL vec = *p == '<'; char *e = NULL;
        unsigned long long n = strtoull(nvmtl_ll_ws(p + 1), &e, 10);
        if (!e || !(q = nvmtl_ll_word(nvmtl_ll_ws(e), "x"))) return NULL;
        nvmtl_ll_layout el; if (!(q = nvmtl_ll_type(q, named, depth + 1, &el))) return NULL;
        q = nvmtl_ll_ws(q); if (*q != (vec ? '>' : ']')) return NULL;
        if (vec) { unsigned long long b = n * el.size; out->align = MIN(nvmtl_ll_pow2(b ?: 1), 128ull); out->size = nvmtl_ll_round(b, out->align); }
        else { out->size = n * el.size; out->align = el.align; }
        p = q + 1;
    } else if (*p == '{' || (*p == '<' && p[1] == '{')) {   // { T, ... } or <{ T, ... }>
        const BOOL packed = *p == '<'; p = nvmtl_ll_ws(p + (packed ? 2 : 1));
        unsigned long long off = 0, al = 1;
        while (*p != '}') {
            nvmtl_ll_layout f; if (!(p = nvmtl_ll_type(p, named, depth + 1, &f))) return NULL;
            if (!packed) { off = nvmtl_ll_round(off, f.align); al = MAX(al, f.align); }
            off += f.size; p = nvmtl_ll_ws(p);
            if (*p == ',') p = nvmtl_ll_ws(p + 1); else if (*p != '}') return NULL;
        }
        p++;
        if (packed) { if (*p != '>') return NULL; p++; }
        out->align = al; out->size = nvmtl_ll_round(off, al);
    } else if (*p == '%') {   // a named struct: %name or %"quoted name"
        const char *s = p + 1;
        if (*s == '"') { const char *c = strchr(s + 1, '"'); if (!c) return NULL; q = c + 1; }
        else { q = s; while (isalnum((unsigned char)*q) || *q == '.' || *q == '_' || *q == '$' || *q == '-') q++; }
        NSString *body = named[[[NSString alloc] initWithBytes:p length:(NSUInteger)(q - p) encoding:NSUTF8StringEncoding] ?: @""];
        if (!body || !nvmtl_ll_type(body.UTF8String, named, depth + 1, out)) return NULL;
        p = q;
    } else if (*p == 'i' && isdigit((unsigned char)p[1])) {
        char *e = NULL; unsigned long long bits = strtoull(p + 1, &e, 10);
        out->size = out->align = nvmtl_ll_pow2((bits + 7) / 8 ?: 1); p = e;
    } else if ((q = nvmtl_ll_word(p, "half")) || (q = nvmtl_ll_word(p, "bfloat"))) { out->size = out->align = 2; p = q; }
    else if ((q = nvmtl_ll_word(p, "float"))) { out->size = out->align = 4; p = q; }
    else if ((q = nvmtl_ll_word(p, "double"))) { out->size = out->align = 8; p = q; }
    else if ((q = nvmtl_ll_word(p, "ptr"))) {
        out->size = out->align = 8; p = q;
        const char *a = nvmtl_ll_ws(p);
        if (!strncmp(a, "addrspace(", 10)) { const char *c = strchr(a, ')'); if (!c) return NULL; p = c + 1; }
    } else return NULL;
    // a typed pointer (older AIR): T*, T addrspace(N)*
    for (;;) {
        const char *a = nvmtl_ll_ws(p);
        if (!strncmp(a, "addrspace(", 10)) { const char *c = strchr(a, ')'); if (!c || *nvmtl_ll_ws(c + 1) != '*') break; a = nvmtl_ll_ws(c + 1); }
        if (*a != '*') break;
        out->size = out->align = 8; p = a + 1;
    }
    return p;
}

// MTLComputePipelineState.staticThreadgroupMemoryLength: the threadgroup memory the function declares itself, its
// program-scope threadgroup variables (AIR `addrspace(3) global`), laid out one after another at their alignment.
// [[threadgroup(n)]] arguments are not counted: their memory is setThreadgroupMemoryLength:atIndex:'s, as on Apple's
// driver. Each function is its own AIR module, so every threadgroup global in the module is the function's (a linked
// pipeline's module also holds its visible functions', which the pipeline allocates too).
NSUInteger nvmtl_air_static_tg(NVMTLFunction *fn)
{
    NSString *ll = fn ? fn->_air : nil;
    if (!ll.length || [ll rangeOfString:@"addrspace(3) global " options:NSLiteralSearch].location == NSNotFound) return 0;
    NSMutableDictionary<NSString *, NSString *> *named = [NSMutableDictionary new];
    NSMutableArray<NSString *> *globals = [NSMutableArray new];
    [ll enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
        if ([line hasPrefix:@"%"]) {
            NSRange t = [line rangeOfString:@" = type " options:NSLiteralSearch];
            if (t.location != NSNotFound) named[[line substringToIndex:t.location]] = [line substringFromIndex:NSMaxRange(t)];
        } else if ([line hasPrefix:@"@"] && [line rangeOfString:@" addrspace(3) global " options:NSLiteralSearch].location != NSNotFound)
            [globals addObject:line];
    }];
    unsigned long long total = 0;
    for (NSString *line in globals) {
        NSRange eq = [line rangeOfString:@" = " options:NSLiteralSearch];
        NSRange g = [line rangeOfString:@" addrspace(3) global " options:NSLiteralSearch];
        if (eq.location == NSNotFound || eq.location > g.location) continue;
        NSString *linkage = [line substringWithRange:NSMakeRange(NSMaxRange(eq), g.location - NSMaxRange(eq))];
        if ([linkage rangeOfString:@"external"].location != NSNotFound) continue;   // a declaration, defined elsewhere
        nvmtl_ll_layout lay; __attribute__((objc_precise_lifetime)) NSString *rest = [line substringFromIndex:NSMaxRange(g)];
        const char *end = nvmtl_ll_type(rest.UTF8String, named, 0, &lay);
        if (!end) { nvlog("static threadgroup memory: %s: the type of %s is not understood — NOT COUNTED", fn->_fname.UTF8String,
                          [line substringToIndex:eq.location].UTF8String); continue; }
        unsigned long long al = lay.align; const char *a = strstr(end, ", align ");
        if (a) { unsigned long long v = strtoull(a + 8, NULL, 10); if (v) al = v; }
        total = nvmtl_ll_round(total, al) + lay.size;
    }
    return (NSUInteger)total;
}

NSArray<NSDictionary *> *nvmtl_argbuf_fields(NVMTLFunction *function, NSUInteger ownerBufferIndex)
{
    NSDictionary *refl = nvmtl_function_reflection(function);
    if (!refl) return nil;
    NSString *name = function->_fname;
    NSArray *all = refl[@"argument_buffer_fields"];
    if (![all isKindOfClass:[NSArray class]]) return nil;
    NSMutableArray *mine = [NSMutableArray new];
    for (NSDictionary *f in all)
        if ([f[@"buffer_index"] unsignedLongValue] == ownerBufferIndex) [mine addObject:f];
    nvlog("argument encoder: %s buffer(%lu) has %lu field(s)", name.UTF8String, (unsigned long)ownerBufferIndex, (unsigned long)mine.count);
    return mine;
}

// The tokens of one AIR metadata tuple `!N = !{a, b, ...}`, commas inside quoted strings kept
// (`!"texture2d<float, sample>"` is one token).
static NSArray<NSString *> *nvmtl_air_tuple_tokens(NSString *line)
{
    NSRange br = line ? [line rangeOfString:@"!{"] : NSMakeRange(NSNotFound, 0);
    if (br.location == NSNotFound) return @[];
    NSMutableArray *tok = [NSMutableArray array]; NSMutableString *cur = [NSMutableString string]; BOOL q = NO;
    for (NSUInteger i = br.location + 2; i < line.length; i++) {
        unichar c = [line characterAtIndex:i];
        if (c == '"') q = !q;
        if (!q && (c == ',' || c == '}')) {
            NSString *t = [cur stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            if (t.length) [tok addObject:t];
            [cur setString:@""];
            if (c == '}') break;
            continue;
        }
        [cur appendFormat:@"%C", c];
    }
    return tok;
}
// YES when a kernel cannot tell how its threads are grouped: every argument is a buffer outside threadgroup memory, a
// texture, a sampler, thread_position_in_grid or threads_per_grid, and the module has no threadgroup memory, barrier,
// SIMD-group or quad operation and no imageblock. The dispatch may then run the same threads in smaller groups.
// Anything the scan does not recognise answers NO, which only costs the reshape.
static BOOL nvmtl_air_grid_only_scan(NSString *ll, NSString *fname)
{
    if (!ll.length || !fname.length) return NO;
    for (NSString *bad in @[@"addrspace(3)", @"@air.wg.barrier", @"@air.simdgroup", @"@air.simd_", @"@air.quad_",
                            @"imageblock", @"@air.atomic.local", @"air.visible_function_table", @"air.intersection_function_table"])
        if ([ll rangeOfString:bad options:NSLiteralSearch].location != NSNotFound) return NO;
    NSArray *allowed = @[@"!\"air.buffer\"", @"!\"air.texture\"", @"!\"air.sampler\"", @"!\"air.thread_position_in_grid\"",
                         @"!\"air.threads_per_grid\""];
    for (NSString *needle in @[[NSString stringWithFormat:@"@%@, ", fname], [NSString stringWithFormat:@"@\"%@\", ", fname]])
    for (NSRange r = NSMakeRange(0, ll.length);;) {
        NSRange m = [ll rangeOfString:needle options:NSLiteralSearch range:r];
        if (m.location == NSNotFound) break;
        NSRange ln = [ll lineRangeForRange:m];
        r = NSMakeRange(NSMaxRange(ln), ll.length - NSMaxRange(ln));
        if ([ll characterAtIndex:ln.location] != '!' || [ll rangeOfString:@" = !{" options:NSLiteralSearch range:ln].location == NSNotFound) continue;
        // `!K = !{ptr @fname, !outputs, !args, ...}`: the third token is the argument list
        NSArray<NSString *> *kt = nvmtl_air_tuple_tokens([ll substringWithRange:ln]);
        if (kt.count < 3 || ![kt[2] hasPrefix:@"!"]) return NO;
        NSRange al = [ll rangeOfString:[NSString stringWithFormat:@"\n%@ = !{", kt[2]] options:NSLiteralSearch];
        if (al.location == NSNotFound) return NO;
        for (NSString *arg in nvmtl_air_tuple_tokens([ll substringWithRange:[ll lineRangeForRange:NSMakeRange(al.location + 1, 1)]])) {
            NSRange an = [ll rangeOfString:[NSString stringWithFormat:@"\n%@ = !{", arg] options:NSLiteralSearch];
            if (an.location == NSNotFound) return NO;
            NSArray<NSString *> *at = nvmtl_air_tuple_tokens([ll substringWithRange:[ll lineRangeForRange:NSMakeRange(an.location + 1, 1)]]);
            if (at.count < 2 || ![allowed containsObject:at[1]]) return NO;
            NSUInteger as = [at indexOfObject:@"!\"air.address_space\""];
            if (as != NSNotFound && as + 1 < at.count && [at[as + 1] isEqualToString:@"i32 3"]) return NO;
        }
        return YES;
    }
    return NO;
}
BOOL nvmtl_air_grid_only(NVMTLFunction *fn)
{
    return fn ? nvmtl_air_grid_only_scan(fn->_air, fn->_fname) : NO;
}

// One `air.struct_type_info` tuple: members of `i32 offset, i32 size, i32 count, !"type", !"name",
// !"air.indirect_argument", leaf`. A leaf is either a node with the member's `air.location_index` (its [[id]] inside the
// struct), or, for a nested struct - whose member starts with `!"air.struct_type_info", !child` - `i32 id`, the
// struct's own [[id]]. An encoder addresses a nested member by its struct's id plus its id inside the struct, at the
// struct's offset plus its own (Metal numbers ImageIO's xdr::colorTransformIn trc.lut 11 at byte 64 this way).
static void nvmtl_air_argbuf_walk(NSDictionary *nodes, NSString *node, NSUInteger baseId, NSUInteger baseOff, NSUInteger depth,
                                  NSUInteger bufferIndex, NSMutableArray *out)
{
    if (depth > 16) return;
    NSArray<NSString *> *tok = nvmtl_air_tuple_tokens(nodes[node]);
    for (NSUInteger i = 0; i < tok.count; ) {
        NSString *child = nil; NSUInteger j = i;
        if ([tok[j] isEqualToString:@"!\"air.struct_type_info\""] && j + 1 < tok.count) { child = tok[j + 1]; j += 2; }
        if (j + 6 >= tok.count || ![tok[j] hasPrefix:@"i32 "] || ![tok[j + 1] hasPrefix:@"i32 "] || ![tok[j + 2] hasPrefix:@"i32 "]
            || ![tok[j + 3] hasPrefix:@"!\""] || ![tok[j + 4] hasPrefix:@"!\""] || tok[j + 4].length < 3 || ![tok[j + 4] hasSuffix:@"\""]
            || ![tok[j + 5] isEqualToString:@"!\"air.indirect_argument\""]) {
            i++; continue;
        }
        const NSUInteger off = (NSUInteger)[[tok[j] substringFromIndex:4] integerValue], size = (NSUInteger)[[tok[j + 1] substringFromIndex:4] integerValue];
        NSString *ref = tok[j + 6], *name = [tok[j + 4] substringWithRange:NSMakeRange(2, tok[j + 4].length - 3)];
        if (child) {
            if ([ref hasPrefix:@"i32 "]) nvmtl_air_argbuf_walk(nodes, child, baseId + (NSUInteger)[[ref substringFromIndex:4] integerValue],
                                                               baseOff + off, depth + 1, bufferIndex, out);
        } else if ([ref hasPrefix:@"!"]) {
            NSString *leaf = nodes[ref]; NSInteger rel = leaf ? nvmtl_air_i32_after(leaf, @"air.location_index") : -1;
            NSString *kind = [leaf rangeOfString:@"!\"air.indirect_constant\""].location != NSNotFound ? @"constant"
                           : [leaf rangeOfString:@"!\"air.texture\""].location != NSNotFound ? @"texture"
                           : [leaf rangeOfString:@"!\"air.sampler\""].location != NSNotFound ? @"sampler" : @"other";
            if (rel >= 0) {
                NSMutableDictionary *fld = [@{ @"buffer_index": @(bufferIndex), @"argument_index": @(baseId + (NSUInteger)rel), @"field_offset": @(baseOff + off),
                                               @"size": @(size), @"kind": kind, @"name": name } mutableCopy];
                // a pointer to another argument buffer (`constant Inner *p [[id(n)]]`): its own struct's members, so that
                // -newArgumentEncoderForBufferAtIndex: can encode the buffer it points at
                if (leaf && [leaf rangeOfString:@"!\"air.indirect_buffer\""].location != NSNotFound) {
                    NSString *inner = nvmtl_air_node_after(leaf, @"air.struct_type_info", 0, NULL);
                    if (inner) {
                        NSMutableArray *nested = [NSMutableArray array];
                        nvmtl_air_argbuf_walk(nodes, inner, 0, 0, depth + 1, bufferIndex, nested);
                        fld[@"nested_fields"] = nested;
                        fld[@"nested_size"] = @(MAX(0, nvmtl_air_i32_after(leaf, @"air.arg_type_size")));
                        fld[@"nested_align"] = @(MAX(0, nvmtl_air_i32_after(leaf, @"air.arg_type_align_size")));
                    }
                }
                [out addObject:fld];
            }
        }
        i = j + 7;
    }
}
// Every member of the argument buffer at [[buffer(bufferIndex)]] as the function's AIR declares it, nested structs
// flattened, and the buffer's size and alignment. The translator's argument_buffer_fields carry only the resources
// a shader reaches; Metal's encoder also covers the constants and the nested structs (ImageIO's HDR converter sizes
// and fills xdr::colorTransformIn, which has no top-level resource, through it).
NSArray<NSDictionary *> *nvmtl_air_argbuf_layout(NVMTLFunction *fn, NSUInteger bufferIndex, NSUInteger *size, NSUInteger *align)
{
    if (size) *size = 0;
    if (align) *align = 0;
    NSString *ll = fn ? fn->_air : nil, *fname = fn ? fn->_fname : nil;
    if (!ll.length || !fname.length) return @[];
    NSMutableDictionary *nodes = [NSMutableDictionary new]; NSString *entryLine = nil; NSRange er = NSMakeRange(NSNotFound, 0);
    NSArray *needles = @[[NSString stringWithFormat:@"@%@, ", fname], [NSString stringWithFormat:@"@\"%@\", ", fname]];
    for (NSString *line in [ll componentsSeparatedByString:@"\n"]) {
        if (![line hasPrefix:@"!"]) continue;
        NSRange eq = [line rangeOfString:@" = !{"]; if (eq.location == NSNotFound) continue;
        nodes[[line substringToIndex:eq.location]] = line;
        if (!entryLine && [line rangeOfString:@"air."].location == NSNotFound)
            for (NSString *needle in needles) { NSRange r = [line rangeOfString:needle]; if (r.location != NSNotFound) { entryLine = line; er = r; break; } }
    }
    if (!entryLine) return @[];
    NSArray *erefs = [[entryLine substringFromIndex:NSMaxRange(er)] componentsSeparatedByString:@", "];
    if (erefs.count < 2) return @[];
    NSString *argsRef = [erefs[1] stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"} "]];
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *ref in nvmtl_air_tuple_tokens(nodes[argsRef])) {
        NSString *line = nodes[ref];
        if ([line rangeOfString:@"!\"air.indirect_buffer\""].location == NSNotFound) continue;
        if (nvmtl_air_i32_after(line, @"air.location_index") != (NSInteger)bufferIndex) continue;
        if (size) *size = (NSUInteger)MAX(0, nvmtl_air_i32_after(line, @"air.arg_type_size"));
        if (align) *align = (NSUInteger)MAX(0, nvmtl_air_i32_after(line, @"air.arg_type_align_size"));
        NSString *st = nvmtl_air_node_after(line, @"air.struct_type_info", 0, NULL);
        if (st) nvmtl_air_argbuf_walk(nodes, st, 0, 0, 0, bufferIndex, out);
        break;
    }
    return out;
}

NSArray<NSDictionary *> *nvmtl_embedded_bindings(NVMTLFunction *function)
{
    if (!function) return nil;
    @synchronized (function) {
        if (function->_embedded) return function->_embedded;
        const char *who = function->_fname.UTF8String ?: "?";
        NSString *ll = function->_air, *stage = function->_stage;
        if (!ll || !stage.length) {
            nvlog("embedded: %s has no AIR to reflect — its argument-buffer resources CANNOT be resolved", who);
            function->_embedded = @[];
            return function->_embedded;
        }
        uint8_t *j = NULL; size_t jn = 0; char e[512] = { 0 };
        if (!nvmtl_xlate_ready() || !xlate_refl) {
            nvlog("embedded: no reflection in this translator — %s's argument-buffer resources CANNOT be resolved", who);
            function->_embedded = @[];
            return function->_embedded;
        }
        (void)j; (void)jn;
        NSData *d = nvmtl_reflect_air_cached(function, ll, stage, e, sizeof e);
        if (!d) {
            nvlog("embedded: reflection for %s FAILED: %s — resources NOT resolved", who, e);
            function->_embedded = @[];
            return function->_embedded;
        }
        NSDictionary *refl = [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL];
        NSArray *all = refl[@"bindings"];
        NSMutableArray *mine = [NSMutableArray new];
        if ([all isKindOfClass:[NSArray class]]) {
            for (NSDictionary *b in all) {
                if (![b isKindOfClass:[NSDictionary class]]) continue;
                NSString *kind = b[@"kind"];
                if ([kind isEqualToString:@"EmbeddedArgBufferTexture"] ||
                    [kind isEqualToString:@"EmbeddedArgBufferSampler"]) [mine addObject:b];
            }
        } else {
            nvlog("embedded: reflection for %s has no bindings array — resources NOT resolved", who);
        }
        if (mine.count) nvlog("embedded: %s reaches %lu resource(s) through argument buffers", who, (unsigned long)mine.count);
        function->_embedded = mine;
        return function->_embedded;
    }
}

NSData *nvmtl_translate_variant_uncached(NVMTLFunction *fn, NSDictionary *states, NSDictionary *tgLen, NSString **err) {
    if (!states.count && !tgLen.count && !g_translate_variant_forced) return fn->_spirv;
    if (!fn->_air || !nvmtl_xlate_ready()) { if(err)*err=@"specialization has no AIR or translator";return nil; }
    if (states.count && !xlate_sampler) { if(err)*err=@"sampler specialization has no translator";return nil; }
    if (tgLen.count && !xlate_tglen) { if(err)*err=@"this translator cannot size threadgroup memory";return nil; }
    uint32_t lens[32] = {0}; uint32_t high = 0;
    for (NSString *k in tgLen) {
        NSInteger idx = k.integerValue; NSUInteger len = [tgLen[k] unsignedIntegerValue];
        if (idx < 0 || idx >= 32) { nvlog("threadgroup index %ld is outside 0..31 — IGNORED", (long)idx); continue; }
        lens[idx] = (uint32_t)len; if ((uint32_t)idx + 1 > high) high = (uint32_t)idx + 1;
    }
    if (high && xlate_tglen) xlate_tglen(lens, high);
    NSData *data = nil; uint8_t *out=NULL; size_t len=0; char e[1024]={0};
    if (states.count) {
        NSData *json=[NSJSONSerialization dataWithJSONObject:@{@"samplers":states,@"constants":fn->_fc ?: @{}} options:NSJSONWritingSortedKeys error:NULL];
        NSString *s=[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
        if(xlate_sampler(fn->_air.UTF8String,fn->_stage.UTF8String,s.UTF8String,&out,&len,e,sizeof e)) { if(err)*err=@(e); }
        else { data=[NSData dataWithBytes:out length:len]; xlate_free(out,len); }
    } else if (fn->_fc.count) {
        if (!xlate_fc) { if(err)*err=@"this translator has no nvmtl_translate_fc \u2014 rebuild nvmtl_translate"; return nil; }
        NSUInteger capacity = fn->_fc.count;
        __attribute__((objc_precise_lifetime)) NSMutableData *packed = nvmtl_fc_storage(capacity);
        uint32_t *idx = packed.mutableBytes, *sz = idx ? idx + capacity : NULL;
        const uint8_t **pay = sz ? (const uint8_t **)(sz + capacity) : NULL;
        uint8_t (*bytes)[16] = pay ? (uint8_t (*)[16])(pay + capacity) : NULL; size_t nfc = 0;
        if (!packed) { if(err)*err=@"function constant variant allocation failed"; if (high && xlate_tglen) xlate_tglen(NULL, 0); return nil; }
        for (NSString *k in fn->_fc) {
            NSArray *a = fn->_fc[k];
            if (![a isKindOfClass:[NSArray class]] || a.count == 0 || a.count > 16) {
                if(err)*err=[NSString stringWithFormat:@"function constant %@ cannot be carried into the threadgroup variant", k];
                if (high && xlate_tglen) xlate_tglen(NULL, 0);
                return nil;
            }
            idx[nfc] = (uint32_t)k.integerValue; sz[nfc] = (uint32_t)a.count;
            for (NSUInteger j = 0; j < a.count; j++) bytes[nfc][j] = (uint8_t)[a[j] unsignedCharValue];
            pay[nfc] = bytes[nfc]; nfc++;
            if (getenv("NVMTL_ZPROBE")) {
                static const char *FH = "0123456789abcdef";
                char fx[40]; size_t fn2 = 0;
                for (uint32_t q = 0; q < sz[nfc-1] && q < 16; q++) {
                    fx[fn2++] = FH[bytes[nfc-1][q] >> 4]; fx[fn2++] = FH[bytes[nfc-1][q] & 15];
                }
                fx[fn2] = 0;
                nvlog("ZFC idx %-4u size %-2u bytes %s", idx[nfc-1], sz[nfc-1], fx);
            }
        }
        if(xlate_fc(fn->_air.UTF8String,fn->_stage.UTF8String,idx,sz,pay,nfc,&out,&len,e,sizeof e)) { if(err)*err=@(e); }
        else { data=[NSData dataWithBytes:out length:len]; xlate_free(out,len);
               nvlog("threadgroup variant: %s re-specialized with %zu constant(s) -> %lu B",
                     fn->_fname.UTF8String, nfc, (unsigned long)data.length); }
    } else {
        if(xlate(fn->_air.UTF8String,fn->_stage.UTF8String,&out,&len,e,sizeof e)) { if(err)*err=@(e); }
        else { data=[NSData dataWithBytes:out length:len]; xlate_free(out,len); }
    }
    if (high && xlate_tglen) xlate_tglen(NULL, 0);
    return data;
}

NSData *nvmtl_translate_variant(NVMTLFunction *fn, NSDictionary *states, NSDictionary *tgLen, NSString **err) {
    if (!states.count && !tgLen.count) return fn->_spirv;
    if (!fn->_air || !nvmtl_xlate_ready()) return nvmtl_translate_variant_uncached(fn,states,tgLen,err);
    NSDictionary *config=@{@"stage":fn->_stage ?: @"",@"entry":fn->_fname ?: @"",
        @"constants":fn->_fc ?: @{},@"samplers":states ?: @{},@"threadgroup":tgLen ?: @{}};
    NSData *json=[NSJSONSerialization dataWithJSONObject:config options:NSJSONWritingSortedKeys error:NULL];
    NVMTLAirDigest *airDigest=nvmtl_air_state(fn->_air);
    if(!json || !airDigest)return nvmtl_translate_variant_uncached(fn,states,tgLen,err);
    NSString *path=nvmtl_spvcache_path_air(airDigest,json,@"variant1");
    if(!path)return nvmtl_translate_variant_uncached(fn,states,tgLen,err);   // no writable cache folder in this process
    NSData *mem=[nvmtl_memo() objectForKey:path];
    if(mem){utimes(path.fileSystemRepresentation,NULL);return mem;}
    NSData *hit=nvmtl_cache_read(path);uint32_t magic=0;
    if(hit.length>=20 && hit.length%4==0){memcpy(&magic,hit.bytes,4);if(magic==0x07230203){[nvmtl_memo() setObject:hit forKey:path cost:hit.length];nvlog("variant cache: hit %s",fn->_fname.UTF8String);return hit;}}
    NSData *result=nvmtl_translate_variant_uncached(fn,states,tgLen,err);
    if(result){if(nvmtl_is_spirv(result))[nvmtl_memo() setObject:result forKey:path cost:result.length];BOOL stored=nvmtl_cache_store(path,result);nvlog("variant cache: miss %s stored=%d",fn->_fname.UTF8String,stored);}
    return result;
}

// a8: Metal A8Unorm colour attachments (bit per location); the variant writes the output's alpha into the R8 lane.
// Sampler-specialised translations are kept in the spvcache too ("samp1", keyed by the AIR and the JSON the translator
// gets): Apple's GL bakes each texture's sampler state into the fragment shader, so every new combination was a full
// translation (20-90 ms) in every session, the first-sight stalls of Minecraft's frames (2026-10-28).
// NVMTL_SAMPCACHE_OFF: translate every time, as before.
static _Atomic unsigned long gSampHit, gSampMiss, gSampStored;
NSData *nvmtl_translate_samplers(NVMTLFunction *fn, NSDictionary *states, uint32_t a8, NSString **err) {
    if (!states.count && !a8) return fn->_spirv;
    if (!fn->_air && !states.count) return fn->_spirv;   // already a specialised variant (no AIR kept): nothing more to apply
    if (!fn->_air || !nvmtl_xlate_ready() || !xlate_sampler) { if(err)*err=@"sampler specialization has no AIR or translator";return nil; }
    NSData *json=[NSJSONSerialization dataWithJSONObject:@{@"samplers":states ?: @{},@"constants":fn->_fc ?: @{},@"a8_targets":@(a8)} options:NSJSONWritingSortedKeys error:NULL];
    NSString *path = nil;
    if (json && !getenv("NVMTL_SAMPCACHE_OFF")) {
        NSMutableData *tail = [json mutableCopy]; const char *st = fn->_stage.UTF8String ?: ""; [tail appendBytes:st length:strlen(st) + 1];
        NVMTLAirDigest *ad = nvmtl_air_state(fn->_air);
        path = ad ? nvmtl_spvcache_path_air(ad, tail, @"samp1") : nil;
    }
    if (path) {
        NSData *mem = [nvmtl_memo() objectForKey:path];
        if (mem) { gSampHit++; utimes(path.fileSystemRepresentation, NULL); return mem; }
        NSData *hit = nvmtl_cache_read(path);
        if (nvmtl_is_spirv(hit)) { gSampHit++; utimes(path.fileSystemRepresentation, NULL); [nvmtl_memo() setObject:hit forKey:path cost:hit.length]; return hit; }
    }
    if (nvmtl_hitch_ms()) nvmtl_hitch_add_ns(NVH_SPVMISS, 0);
    NSString *s=[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
    uint8_t *out=NULL;size_t len=0;char e[1024]={0};
    const uint64_t t0 = nvmtl_hitch_t0(), t1 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    if(xlate_sampler(fn->_air.UTF8String,fn->_stage.UTF8String,s.UTF8String,&out,&len,e,sizeof e)) {if(err)*err=@(e);return nil;}
    nvmtl_hitch_add(NVH_XLATESAMP, t0, 0);
    nvmtl_xlate_dump("samplers", fn->_air.UTF8String, fn->_stage.UTF8String, t1, len);
    NSData *data=[NSData dataWithBytes:out length:len];xlate_free(out,len);
    const unsigned long miss = ++gSampMiss;
    if (path && nvmtl_is_spirv(data)) {
        [nvmtl_memo() setObject:data forKey:path cost:data.length];
        if (nvmtl_cache_store(path, data)) gSampStored++;
    }
    if (miss <= 4 || (miss & (miss - 1)) == 0)
        nvlog("sampler cache: miss %s (%s) %lu B [%lu hit, %lu miss, %lu stored]%s", fn->_fname.UTF8String, fn->_stage.UTF8String,
              (unsigned long)data.length, (unsigned long)gSampHit, miss, (unsigned long)gSampStored, path ? "" : " - no cache folder");
    return data;
}

#pragma mark - batch 32: visible-function linking
static NSError *nvmtl_link_err(NSString *m) {
    /* "FAILED" keeps this line in the release syslog (nvmtl_log_failure): a refused link makes the caller's
     * pipeline nil, and RenderBox only reports that as a <private> "precondition failure". */
    nvlog("link FAILED: %s", m.UTF8String);
    return [NSError errorWithDomain:@"NVMTL" code:4 userInfo:@{NSLocalizedDescriptionKey: m}];
}

/* MTLFunctionDescriptor.specializedName renames the function it creates, and a linked caller then refers
 * to it by that new name. RenderBox does exactly this for every custom shader: it asks for IconRendering's
 * glassHighlight_v1 / glow_v1 / sdfFill_v1 / shapeAwareGradientMask_v1 / clampToEdges_v1 (and its own
 * distanceGradient_v1 ...) with specializedName "custom_fn" and links it as a privateFunction into
 * primitive_/accumulator_/filter_custom_fragment and custom_effect_fragment, whose AIR references
 * !"custom_fn". The AIR we keep still defines @glassHighlight_v1, and the translator's validate_linkage()
 * refuses any symbol its module does not define ("linked module does not define authored visible function
 * reference \"custom_fn\""), so every such pipeline failed and the icon effect was never drawn.
 * These helpers mirror linked_functions.rs llvm_global()/module_defines(). */
static BOOL nvmtl_llvm_plain_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$' || c == '-';
}
static NSString *nvmtl_llvm_global_name(NSString *sym) {
    const char *s = sym.UTF8String;
    if (!s || !*s) return nil;
    BOOL plain = YES;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\n' || *p == '\r') return nil;
        if (!nvmtl_llvm_plain_char(*p)) plain = NO;
    }
    if (plain) return [@"@" stringByAppendingString:sym];
    NSString *q = [[sym stringByReplacingOccurrencesOfString:@"\\" withString:@"\\5C"] stringByReplacingOccurrencesOfString:@"\"" withString:@"\\22"];
    return [NSString stringWithFormat:@"@\"%@\"", q];
}
static BOOL nvmtl_air_defines(NSString *ll, NSString *sym) {
    NSString *g = nvmtl_llvm_global_name(sym);
    if (!g || !ll.length) return NO;
    NSString *call = [g stringByAppendingString:@"("];
    __block BOOL found = NO;
    [ll enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
        NSUInteger i = 0, n = line.length;
        while (i < n && ([line characterAtIndex:i] == ' ' || [line characterAtIndex:i] == '\t')) i++;
        if (n - i > 7 && [line compare:@"define " options:NSLiteralSearch range:NSMakeRange(i, 7)] == NSOrderedSame &&
            [line rangeOfString:call options:NSLiteralSearch].location != NSNotFound) { found = YES; *stop = YES; }
    }];
    return found;
}
/* Every whole-token use of global `from` (definition, calls, metadata such as !air.visible) becomes `to`.
 * nil when `to` is already a global of this module (nothing is renamed then). */
static NSString *nvmtl_air_rename_global(NSString *ll, NSString *from, NSString *to) {
    NSString *gf = nvmtl_llvm_global_name(from), *gt = nvmtl_llvm_global_name(to);
    if (!gf || !gt || !ll.length) return nil;
    BOOL quoted = [gf hasPrefix:@"@\""];
    NSMutableString *outs = [NSMutableString stringWithCapacity:ll.length + 64];
    NSUInteger pos = 0, n = ll.length, hits = 0;
    for (;;) {
        NSRange r = [ll rangeOfString:gf options:NSLiteralSearch range:NSMakeRange(pos, n - pos)];
        if (r.location == NSNotFound) break;
        NSUInteger end = NSMaxRange(r);
        unichar next = end < n ? [ll characterAtIndex:end] : 0;
        BOOL whole = quoted || end >= n || next >= 128 || !nvmtl_llvm_plain_char((unsigned char)next);
        [outs appendString:[ll substringWithRange:NSMakeRange(pos, r.location - pos)]];
        [outs appendString:whole ? gt : gf];
        if (whole) hits++;
        pos = end;
    }
    if (!hits) return nil;
    [outs appendString:[ll substringFromIndex:pos]];
    if (nvmtl_air_defines(ll, to)) return nil;
    return outs;
}
/* The AIR a linked function contributes, under the name its callers use. */
static NSString *nvmtl_link_candidate_air(NVMTLFunction *cf, NSString **nameOut) {
    NSString *air = cf->_air, *name = cf->_fname;
    if (cf->_specName.length && cf->_fname.length && ![cf->_specName isEqualToString:cf->_fname]) {
        if (nvmtl_air_defines(air, cf->_specName)) name = cf->_specName;
        else {
            NSString *renamed = nvmtl_air_defines(air, cf->_fname) ? nvmtl_air_rename_global(air, cf->_fname, cf->_specName) : nil;
            if (renamed) { air = renamed; name = cf->_specName;
                nvlog("link: \"%s\" renamed to its specializedName \"%s\"", cf->_fname.UTF8String, cf->_specName.UTF8String); }
            else nvlog("link: specializedName \"%s\" of \"%s\" could not be applied - FAILED to rename its AIR", cf->_specName.UTF8String, cf->_fname.UTF8String);
        }
    }
    if (nameOut) *nameOut = name;
    return air;
}
// A compute pipeline with a stage-input descriptor reads its [[stage_in]] attributes from the application's buffer by that
// descriptor's strides, offsets and formats, so its kernel is translated again with the layout (the kernel's own SPIR-V reads
// each member as an array of its own type at its natural size). A kernel with no stage-in attribute, or a descriptor that
// names none, keeps the function it has. NVMTL_NO_KERNEL_STAGE_IN=1 turns this off. A layout the translator cannot carry
// (a format or step function outside its table, an unaligned stride) fails the pipeline with the reason instead of
// running it with the wrong strides.
static NSError *nvmtl_stagein_err(NSString *m) {
    nvlog("%s", m.UTF8String);
    return [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorCompileFailure userInfo:@{NSLocalizedDescriptionKey: m}];
}
static NSString *nvmtl_stage_in_words(MTLStageInputOutputDescriptor *sd, NSMutableData *words) {
    uint32_t a[1 + 16 * 4 + 1 + 31 * 4]; uint32_t n = 1, na = 0; uint32_t bufs[31]; uint32_t nb = 0;
    for (NSUInteger i = 0; i < 16; i++) {
        MTLAttributeDescriptor *at = [sd.attributes objectAtIndexedSubscript:i];
        if (at.format == MTLAttributeFormatInvalid) continue;
        if (at.bufferIndex >= 31) return [NSString stringWithFormat:@"stage-in attribute %lu uses buffer %lu", (unsigned long)i, (unsigned long)at.bufferIndex];
        a[n++] = (uint32_t)i; a[n++] = (uint32_t)at.bufferIndex; a[n++] = (uint32_t)at.offset; a[n++] = (uint32_t)at.format; na++;
        BOOL seen = NO; for (uint32_t k = 0; k < nb; k++) if (bufs[k] == at.bufferIndex) seen = YES;
        if (!seen) bufs[nb++] = (uint32_t)at.bufferIndex;
    }
    if (!na) return nil;
    a[0] = na;
    a[n++] = nb;
    for (uint32_t k = 0; k < nb; k++) {
        MTLBufferLayoutDescriptor *ly = [sd.layouts objectAtIndexedSubscript:bufs[k]];
        a[n++] = bufs[k]; a[n++] = (uint32_t)ly.stride; a[n++] = (uint32_t)ly.stepFunction; a[n++] = (uint32_t)ly.stepRate;
    }
    [words appendBytes:a length:n * sizeof a[0]];
    return nil;
}

NVMTLFunction *nvmtl_kernel_with_stage_in(NVMTLFunction *fn, MTLStageInputOutputDescriptor *sd, NSError **err)
{
    if (err) *err = nil;
    if (!sd || !fn || getenv("NVMTL_NO_KERNEL_STAGE_IN")) return fn;
    if (!fn->_air.length || fn->_needsLink) return fn;     // linked kernels were translated by the linker; not carried here
    NSMutableData *words = [NSMutableData new];
    NSString *bad = nvmtl_stage_in_words(sd, words);
    if (bad) { if (err) *err = nvmtl_stagein_err([NSString stringWithFormat:@"compute stage-in: %@", bad]); return nil; }
    if (!words.length) return fn;
    if (!nvmtl_xlate_ready() || !xlate_stagein) {
        if (err) *err = nvmtl_stagein_err(@"compute stage-in: this translator has no nvmtl_translate_set_kernel_stage_in - rebuild nvmtl_translate");
        return nil;
    }
    NSString *key = [NSString stringWithFormat:@"stagein:%lu:%@:%@:%@", (unsigned long)fn->_air.hash, fn->_fname, [words base64EncodedStringWithOptions:0], fn->_fc ?: @""];
    static NSMutableDictionary *memo; static dispatch_once_t once; dispatch_once(&once, ^{ memo = [NSMutableDictionary new]; });
    NSData *spirv;
    @synchronized (memo) { spirv = memo[key]; }
    if (!spirv) {
        if (!xlate_stagein(words.bytes, (uint32_t)(words.length / 4))) {
            if (err) *err = nvmtl_stagein_err(@"compute stage-in: the translator refused the stage-input layout (a format, step function or count it does not carry)");
            return nil;
        }
        NSString *why = nil;
        g_translate_variant_forced = YES;
        spirv = nvmtl_translate_variant_uncached(fn, @{}, @{}, &why);
        g_translate_variant_forced = NO;
        xlate_stagein(NULL, 0);
        if (!spirv) {
            if (err) *err = nvmtl_stagein_err([NSString stringWithFormat:@"compute stage-in: translating \"%@\" with the layout failed: %@", fn->_fname, why ?: @"unknown"]);
            return nil;
        }
        @synchronized (memo) { memo[key] = spirv; }
        nvlog("compute stage-in: \"%s\" translated with %lu word(s) of layout -> %lu B", fn->_fname.UTF8String, (unsigned long)(words.length / 4), (unsigned long)spirv.length);
    }
    NVMTLFunction *sf = [NVMTLFunction new];
    sf->_spirv = spirv; sf->_fname = fn->_fname; sf->_stage = fn->_stage; sf->_air = fn->_air; sf->_fc = fn->_fc;
    sf->_lib = fn->_lib; sf->_fcv = fn->_fcv; sf->_specName = fn->_specName; sf->_needsLink = NO;
    return sf;
}

id<MTLFunction> nvmtl_link_kernel(MTLComputePipelineDescriptor *d, NSError **err)
{
    return nvmtl_link_stage(d.computeFunction, d.linkedFunctions, d.preloadedLibraries, err);
}

#pragma mark - batch 41: dynamic libraries
@interface NVMTLDynamicLibrary : NSObject <MTLDynamicLibrary> { @public NVMTLLibrary *_lib; NSString *_installName, *_label; }
- (dispatch_data_t)airData NS_RETURNS_RETAINED;
@end
static char kNVMTLCompileDylibs;
static char kNVMTLRenderVert, kNVMTLRenderFrag;

static BOOL nvmtl_mtlb_dynamic(NSData *raw, NSString **nameOut, NSArray<NSString *> **importsOut)
{
    if (nameOut) *nameOut = nil;
    if (importsOut) *importsOut = @[];
    const uint8_t *b = raw.bytes; NSUInteger n = raw.length;
    if (n < 0x58 || memcmp(b, "MTLB", 4) != 0) return NO;
    uint64_t flo = 0, fls = 0; memcpy(&flo, b + 0x18, 8); memcpy(&fls, b + 0x20, 8);
    if (flo > n || fls > n - flo) return NO;
    NSUInteger pos = (NSUInteger)(flo + fls); uint64_t doff = 0, dsz = 0; BOOL have = NO;
    while (pos + 6 <= n && memcmp(b + pos, "ENDT", 4) != 0) {
        uint16_t len = 0; memcpy(&len, b + pos + 4, 2);
        if (pos + 6 + len > n) return NO;
        if (memcmp(b + pos, "HDYN", 4) == 0 && len == 16) { memcpy(&doff, b + pos + 6, 8); memcpy(&dsz, b + pos + 14, 8); have = YES; }
        pos += 6 + len;
    }
    if (!have) return YES;
    if (doff > n || dsz > n - doff) return NO;
    NSUInteger p = (NSUInteger)doff, end = (NSUInteger)(doff + dsz);
    NSMutableArray<NSString *> *imp = [NSMutableArray new];
    while (p + 6 <= end && memcmp(b + p, "ENDT", 4) != 0) {
        uint16_t len = 0; memcpy(&len, b + p + 4, 2);
        if (p + 6 + len > end) break;
        BOOL isName = memcmp(b + p, "NAME", 4) == 0, isDyn = memcmp(b + p, "DYNL", 4) == 0;
        if ((isName || isDyn) && len > 1 && b[p + 6 + len - 1] == 0) {
            NSString *s = [[NSString alloc] initWithBytes:b + p + 6 length:len - 1 encoding:NSUTF8StringEncoding];
            if (s && isName && nameOut) *nameOut = s;
            if (s && isDyn) [imp addObject:s];
        }
        p += 6 + len;
    }
    if (importsOut) *importsOut = [imp copy];
    return YES;
}

static NSError *nvmtl_b41_err(NSString *m) {
    nvlog("%s", m.UTF8String);
    return [NSError errorWithDomain:@"NVMTL" code:5 userInfo:@{NSLocalizedDescriptionKey: m}];
}

static NVMTLFunction *nvmtl_link_dylibs(NVMTLFunction *fn, NSArray *dylibs, NSError **err)
{
    NSMutableSet<NSString *> *decl = [NSMutableSet new], *def = [NSMutableSet new];
    [fn->_air enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
        BOOL isDecl = [line hasPrefix:@"declare"];
        if (!isDecl && ![line hasPrefix:@"define"]) return;
        NSString *s = nvmtl_sym_at(line);
        if (!s.length || [s hasPrefix:@"air."] || [s hasPrefix:@"llvm."]) return;
        [(isDecl ? decl : def) addObject:s];
    }];
    [decl minusSet:def];
    if (!decl.count) return fn;
    if (!xlate_link_ext) { if (err) *err = nvmtl_b41_err(@"dylib: this translator has no nvmtl_link_externs - rebuild nvmtl_translate-merge with patch_xl_extern.py"); return nil; }
    NSMutableArray<NVMTLLibrary *> *libs = [NSMutableArray new];
    NSMutableString *from = [NSMutableString new];
    NSMutableArray *offered = [NSMutableArray arrayWithArray:dylibs ?: @[]];
    NSArray *compiled = fn->_lib ? objc_getAssociatedObject(fn->_lib, &kNVMTLCompileDylibs) : nil;
    [offered addObjectsFromArray:compiled ?: @[]];
    for (id dl in offered) {
        if (![dl isKindOfClass:[NVMTLDynamicLibrary class]]) { [from appendFormat:@" [foreign %@ - skipped]", NSStringFromClass([dl class])]; continue; }
        NVMTLDynamicLibrary *x = dl;
        if (x->_lib && [libs indexOfObjectIdenticalTo:x->_lib] == NSNotFound) { [libs addObject:x->_lib]; [from appendFormat:@" [%@]", x->_installName ?: @"(no installName)"]; }
    }
    NSArray<NSString *> *imports = nil;
    NVMTLLibrary *owner = [fn->_lib isKindOfClass:[NVMTLLibrary class]] ? (NVMTLLibrary *)fn->_lib : nil;
    if (owner) nvmtl_mtlb_dynamic(owner->_raw, NULL, &imports);
    for (NSString *path in imports) {
        BOOL dup = NO;
        for (id dl in offered) if ([dl isKindOfClass:[NVMTLDynamicLibrary class]] && [((NVMTLDynamicLibrary *)dl)->_installName isEqualToString:path]) dup = YES;
        if (dup) continue;
        if (![[NSFileManager defaultManager] fileExistsAtPath:path]) { [from appendFormat:@" [installName %@ NOT ON DISK]", path]; continue; }
        NSError *le = nil;
        id l = [(id<MTLDevice>)gNVMTLMainDevice newLibraryWithURL:[NSURL fileURLWithPath:path] error:&le];
        if ([l isKindOfClass:[NVMTLLibrary class]]) { [libs addObject:l]; [from appendFormat:@" [disk %@]", path]; }
        else [from appendFormat:@" [disk %@ UNREADABLE: %@]", path, le.localizedDescription ?: @"?"];
    }
    NSMutableArray<NSString *> *mods = [NSMutableArray new];
    for (NVMTLLibrary *l in libs) for (NSString *k in l->_airs) { NSString *m = l->_airs[k]; if ([mods indexOfObjectIdenticalTo:m] == NSNotFound) [mods addObject:m]; }
    NSString *want = [[decl.allObjects sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@","];
    size_t n = mods.count;
    const char **mls = calloc(n ? n : 1, sizeof *mls);
    if (!mls) { if (err) *err = nvmtl_b41_err(@"dylib: module table allocation failed"); return nil; }
    for (size_t i = 0; i < n; i++) mls[i] = mods[i].UTF8String;
    uint8_t *out = NULL; size_t len = 0; char e[1024] = {0};
    int rc = xlate_link_ext(fn->_air.UTF8String, mls, n, &out, &len, e, sizeof e);
    free(mls);
    if (rc != 0 || !out) {
        if (err) *err = nvmtl_b41_err([NSString stringWithFormat:@"dylib: \"%@\" needs %@ - %s; searched%@", fn->_fname, want, e[0] ? e : "link failed", from.length ? from : @" nothing (no preloadedLibraries, no compile-time libraries, no DYNL)"]);
        return nil;
    }
    NSString *linked = [[NSString alloc] initWithBytes:out length:(len && out[len-1] == 0) ? len - 1 : len encoding:NSUTF8StringEncoding];
    xlate_free(out, len);
    if (!linked) { if (err) *err = nvmtl_b41_err(@"dylib: linked AIR is not UTF-8"); return nil; }
    nvlog("dylib: \"%s\" extern(s) %s resolved from %zu module(s):%s (%lu -> %lu B AIR)", fn->_fname.UTF8String, want.UTF8String, n,
          from.UTF8String, (unsigned long)fn->_air.length, (unsigned long)linked.length);
    NVMTLFunction *df = [NVMTLFunction new];
    df->_spirv = nil; df->_fname = fn->_fname; df->_stage = fn->_stage; df->_air = linked; df->_fc = fn->_fc;
    df->_lib = fn->_lib; df->_fcv = fn->_fcv; df->_specName = fn->_specName; df->_needsLink = YES;
    return df;
}

// Whether this AIR reads a visible or an intersection function table. Both callers below used to decide that by
// searching for the argument metadata string !"air.visible_function_table", which a ray-tracing kernel from
// Apple's shader converter never carries: its table is a field of the dispatch-rays argument struct, not a
// kernel argument, so the only trace of it in the module is the intrinsic that reads it. Measured over 76
// ray-tracing kernels captured from a running game: 76 of 76 call
// air.get_function_pointer_visible_function_table, 0 of 76 carry the metadata either caller looked for. The
// table branch was therefore never taken, the plain link ran with no table candidates, the handle lookup
// survived specialization, and translation refused the module on the null compare its result feeds - 76
// refusals and not one "reads a visible function table" line in the run. Ask for the intrinsics, which are what
// actually reads the table; the metadata stays as a second form so a module that does carry it still matches.
BOOL nvmtl_air_reads_function_table(NSString *air)
{
    if (!air.length) return NO;
    static NSArray<NSString *> *forms;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        forms = @[@"@air.get_function_pointer_visible_function_table",
                  @"@air.get_size_visible_function_table",
                  @"@air.is_null_visible_function_table",
                  @"@air.get_null_intersection_function_table",
                  @"@air.set_buffer_intersection_function_table",
                  @"@air.set_intersection_function_table",
                  @"!\"air.visible_function_table\"",
                  @"!\"air.intersection_function_table\""];
    });
    for (NSString *f in forms)
        if ([air rangeOfString:f].location != NSNotFound) return YES;
    return NO;
}

// Whether a function needs linking before it can be translated: it calls visible functions it does not define,
// so the pipeline's linked functions have to supply them. This used to be decided by reading the translator's
// error text, and when the translator stopped refusing every visible-function reference outright, its message for
// this case changed ("is called on a live path but no linked function supplies it"). Nothing matched the new text,
// so the function was never deferred to pipeline creation; it came back nil and the caller drew nothing. On macOS
// 26 that is the system's icon and Liquid Glass shaders: icons missing pieces, glass menus drawn as a flat tint.
// The translator lists the references itself, so ask it. The known messages stay as a fallback for a translator
// that cannot answer, so a query that cannot run never reads as "no references".
BOOL nvmtl_needs_visible_link(NSString *air, NSString *why)
{
    if (air.length && nvmtl_xlate_ready() && xlate_vrefs) {
        uint8_t *out = NULL; size_t len = 0; char e[256] = {0};
        if (xlate_vrefs(air.UTF8String, &out, &len, e, sizeof e) == 0) {
            id refs = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:out length:len] options:0 error:NULL];
            xlate_free(out, len);
            // the query lists direct references only; a function that calls through a visible or intersection
            // function table has none, and nvmtl_link_stage links it from the table candidates
            if ([refs isKindOfClass:[NSArray class]])
                return [(NSArray *)refs count] > 0 || nvmtl_air_reads_function_table(air);
        }
    }
    return why && ([why rangeOfString:@"visible function reference"].location != NSNotFound
                || [why rangeOfString:@"Metal visible function table"].location != NSNotFound
                || [why rangeOfString:@"no linked function supplies it"].location != NSNotFound
                || [why rangeOfString:@".MTL_UNRESOLVED_VISIBLE_FN"].location != NSNotFound);
}

// nvmtl_needs_visible_link(air, nil) once per AIR string (f94cc94's early deferral asks for every constant-value variant;
// the reference query parses the whole module). The library hands out the same NSString for a name, so the key costs a
// pointer compare; an equal string from elsewhere hashes and compares as usual.
BOOL nvmtl_needs_visible_link_once(NSString *air)
{
    if (!air.length) return NO;
    static NSCache *c; static dispatch_once_t once;
    dispatch_once(&once, ^{ c = [NSCache new]; c.countLimit = 1024; });
    NSNumber *v = [c objectForKey:air];
    if (!v) { v = @(nvmtl_needs_visible_link(air, nil)); [c setObject:v forKey:air]; }
    return v.boolValue;
}

// A link that failed (the linker or the translation refused it) fails the same way for the same inputs. Since a refused
// visible-function module is deferred to the pipeline link (nvmtl_needs_visible_link), a module that really is broken would otherwise be
// linked and translated again for every pipeline that uses it. The key holds the inputs themselves: equal strings compare
// by pointer first, so a lookup costs no AIR comparison when the library hands out the same objects.
static NSCache *nvmtl_link_failures(void) {
    static NSCache *c; static dispatch_once_t once;
    dispatch_once(&once, ^{ c = [NSCache new]; c.countLimit = 256; });
    return c;
}
static id nvmtl_link_failure_key(NVMTLFunction *fn, NSDictionary *mods, NSArray *tc, NSDictionary *fc) {
    return @[fn->_air ?: @"", fn->_stage ?: @"", mods ?: @{}, tc ?: @[], fc ?: @{}];
}
// The functions a stage's visible function tables can reach, in table order: the descriptor's linked functions, then its
// binary functions (MTLFunctionOptionCompileToBinary; Metal 4 binary functions; those added to a pipeline later through
// newComputePipelineStateWithAdditionalBinaryFunctions come last). A handle's ID is the function's place in this list
// plus one, so a function added later never moves the ones before it. A function listed twice counts once.
NSArray *nvmtl_link_table_list(MTLLinkedFunctions *lf)
{
    NSMutableArray *all = [NSMutableArray new];
    for (NSArray *src in @[lf.functions ?: @[], lf.binaryFunctions ?: @[]])
        for (id c in src) if (![all containsObject:c]) [all addObject:c];
    return all;
}
// The same list with only the functions that have AIR: the ones a handle can name.
NSArray *nvmtl_link_handle_list(MTLLinkedFunctions *lf)
{
    NSMutableArray *out = [NSMutableArray new];
    for (id c in nvmtl_link_table_list(lf))
        if ([c isKindOfClass:[NVMTLFunction class]] && ((NVMTLFunction *)c)->_air.length) [out addObject:c];
    return out;
}
id<MTLFunction> nvmtl_link_stage(id<MTLFunction> kIn, MTLLinkedFunctions *lfIn, NSArray *dylibs, NSError **err)
{
    if (err) *err = nil;
    id<MTLFunction> k = kIn;
    if (![(id)k isKindOfClass:[NVMTLFunction class]]) return k;
    NVMTLFunction *fn = (NVMTLFunction *)k;
    if (fn->_air.length && (dylibs.count || [fn->_air rangeOfString:@"@air.dyld_lib_table"].location != NSNotFound)) {
        NSError *de = nil; NVMTLFunction *df = nvmtl_link_dylibs(fn, dylibs, &de);
        if (!df) { if (err) *err = de; return nil; }
        fn = df;
    }
    // A PIPELINE'S LINKED FUNCTIONS MUST BE RECORDED EVEN WHEN NOTHING NEEDS LINKING.
    //    This early return skips the whole link - correctly, there is nothing to resolve - but it also
    //    threw away the descriptor's linkedFunctions, so the pipeline never learned them and
    //    functionHandleWithFunction had nothing to answer from. MEASURED: the game creates
    //    rgs_reference_wavefront_trace with needsLink=no and 24 linkedFunctions, then asks that pipeline
    //    for handles to ahs_main / chs_main_0..5 and gets nil for all 8 - after which it binds no visible
    //    function table, no intersection table and no acceleration structure, which is where ray tracing
    //    stopped. A shader that dispatches only through handles the HOST writes needs no link of its own.
    // The list is attached to the object the pipeline will hold (`k`), because that is what
    //    functionHandleWithFunction reads; only functions with AIR are recorded, since a handle is a
    //    name-to-index mapping and an AIR-less candidate could never be dispatched anyway.
    if (!fn->_needsLink) {
        NSArray *handleFns = nvmtl_link_handle_list(lfIn);
        if (handleFns.count && !objc_getAssociatedObject(k, &kNVMTLTableFns)) {
            objc_setAssociatedObject(k, &kNVMTLTableFns, [handleFns copy], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            FILE *lg = fopen(nvmtl_log_path(), "a");
            if (lg) {
                fprintf(lg, "pid %d NVMTL: link: \"%s\" needs no link but carries %lu linked function(s) for its handles\n",
                        getpid(), fn->_fname.UTF8String, (unsigned long)handleFns.count);
                fclose(lg);
            }
        }
        return k;
    }
    if (!nvmtl_xlate_ready() || !xlate_vrefs || !xlate_link) {
        if (err) *err = nvmtl_link_err(@"this translator has no nvmtl_link_visible - rebuild nvmtl_translate-merge with patch_xl_link.py");
        return nil;
    }
    uint8_t *out = NULL; size_t len = 0; char e[1024] = {0};
    NSArray *refs = nil;
    if (xlate_vrefs(fn->_air.UTF8String, &out, &len, e, sizeof e) == 0) {
        refs = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:out length:len] options:0 error:NULL];
        xlate_free(out, len); out = NULL;
    }
    if (![refs isKindOfClass:[NSArray class]]) {
        if (err) *err = nvmtl_link_err([NSString stringWithFormat:@"\"%@\": cannot read its visible references (%s)", fn->_fname, e]);
        return nil;
    }
    NSMutableArray *cands = [NSMutableArray new];
    MTLLinkedFunctions *lf = lfIn;
    // PER-LINK, WITH THE ENTRY NAME. computePipelineDescriptor's line is deduped by format
    //    string (nvlog_dropped), so after a few cs_main_ pipelines every later one is suppressed and the
    //    RT entries' counts never appear. This says what THIS link was actually given, which is the only
    //    way to tell "the game passed no linked functions" from "we were called on a path that drops
    //    them" - rgs_reference_wavefront_trace gets 0 candidates and then 8 nil handle requests.
    // AND IT MUST BYPASS nvlog: nvlog_dropped dedupes by FORMAT-STRING POINTER, keeping the first
    //    few per call site and then every Nth, so one busy site hides everything after it. That is why
    //    the descriptor line only ever showed cs_main_ - the RT pipelines came later and were dropped -
    //    and it would silently do the same here, which already made one run read as "never linked".
    {
        FILE *lg = fopen(nvmtl_log_path(), "a");
        if (lg) {
            fprintf(lg, "pid %d NVMTL: link: \"%s\" given linkedFunctions=%s functions=%lu private=%lu binary=%lu groups=%lu, %lu dylib(s)\n",
                    getpid(), ((NVMTLFunction *)kIn)->_fname.UTF8String, lf ? "YES" : "nil",
                    (unsigned long)lf.functions.count,
                    (unsigned long)([lf respondsToSelector:@selector(privateFunctions)]
                                    ? [(NSArray *)[(id)lf valueForKey:@"privateFunctions"] count] : 0),
                    (unsigned long)lf.binaryFunctions.count, (unsigned long)lf.groups.count,
                    (unsigned long)[dylibs count]);
            fclose(lg);
        }
    }
    [cands addObjectsFromArray:nvmtl_link_table_list(lf)];   // the linked functions and the binary functions
    if ([lf respondsToSelector:@selector(privateFunctions)]) { NSArray *pf = [(id)lf valueForKey:@"privateFunctions"]; if ([pf isKindOfClass:[NSArray class]]) [cands addObjectsFromArray:pf]; }
    for (NSArray *g in lf.groups.allValues) if ([g isKindOfClass:[NSArray class]]) [cands addObjectsFromArray:g];
    NSMutableDictionary<NSString *, NSString *> *mods = [NSMutableDictionary new];
    NSMutableDictionary *fc = [NSMutableDictionary new];
    NSMutableString *seen = [NSMutableString new];
    for (id c in cands) {
        if (![c isKindOfClass:[NVMTLFunction class]]) { [seen appendFormat:@" [foreign %@ %@]", NSStringFromClass([c class]), [c name]]; continue; }
        NVMTLFunction *cf = (NVMTLFunction *)c;
        [seen appendFormat:@" [%@%@%@ air=%lu fc=%lu]", cf->_fname, cf->_specName ? @"/" : @"", cf->_specName ?: @"",
            (unsigned long)cf->_air.length, (unsigned long)cf->_fc.count];
        if (!cf->_air.length) continue;
        NSString *air = nvmtl_link_candidate_air(cf, NULL);
        /* Only names the module really defines: validate_linkage() refuses the whole link otherwise. */
        for (NSString *nm in @[cf->_specName ?: @"", cf.name ?: @"", cf->_fname ?: @""])
            if (nm.length && !mods[nm] && nvmtl_air_defines(air, nm)) mods[nm] = air;
        for (NSString *ki in cf->_fc) {
            if (fc[ki] && ![fc[ki] isEqual:cf->_fc[ki]]) nvlog("link: constant %s differs between linked functions - first kept", ki.UTF8String);
            else fc[ki] = cf->_fc[ki];
        }
    }
    [fc addEntriesFromDictionary:fn->_fc ?: @{}];
    nvlog("link: \"%s\" references %s; %lu linked function(s):%s", fn->_fname.UTF8String,
          [[refs componentsJoinedByString:@","] UTF8String], (unsigned long)cands.count, seen.UTF8String);
    NSMutableArray *missing = [NSMutableArray new];
    for (NSString *s in refs) {
        if (![s isKindOfClass:[NSString class]]) {
            if (err) *err = nvmtl_link_err([NSString stringWithFormat:@"\"%@\": malformed visible reference %@", fn->_fname, [s description]]);
            return nil;
        }
        if (!mods[s]) [missing addObject:s];
    }
    if (missing.count)
        nvlog("link: \"%s\": no linked function supplies %s - left unresolved; translation refuses it if a call survives the constants",
              fn->_fname.UTF8String, [[missing componentsJoinedByString:@", "] UTF8String]);
    NSArray *keys = mods.allKeys; size_t n = keys.count;
    const char **syms = calloc(n ? n : 1, sizeof *syms), **mls = calloc(n ? n : 1, sizeof *mls);
    if (!syms || !mls) { free(syms); free(mls); if (err) *err = nvmtl_link_err(@"link table allocation failed"); return nil; }
    for (size_t i = 0; i < n; i++) { syms[i] = [keys[i] UTF8String]; mls[i] = [mods[keys[i]] UTF8String]; }
    // A REFERENCED FUNCTION CAN BE THE ONE THAT READS THE TABLE, and testing only the entry's
    //    own AIR missed every one of them. MEASURED on the game's refused RT kernels: 60 of 60 surviving
    //    `air.get_function_pointer_visible_function_table` calls live in a LINKED-IN define, not in the
    //    entry — so this returned NO, the plain xlate_link path ran, it pulled that function in, and the
    //    lookup reached the emitter, where `icmp eq ptr %fp, null` refuses with "pointer nullness is not
    //    tracked". The table path has to be chosen on what the LINKED module will contain, which is the
    //    entry plus every module linked into it.
    BOOL usesTable = nvmtl_air_reads_function_table(fn->_air);
    if (!usesTable) {
        for (NSString *s in mods) {
            if (nvmtl_air_reads_function_table(mods[s])) {
                usesTable = YES;
                nvlog("link: \"%s\": the table is read by linked function %s, not by the entry - taking the visible-table path",
                      fn->_fname.UTF8String, s.UTF8String);
                break;
            }
        }
    }
    id failKey = nvmtl_link_failure_key(fn, mods, usesTable ? nvmtl_link_table_list(lf) : @[], fc);
    NSString *known = [nvmtl_link_failures() objectForKey:failKey];
    if (known) {
        free(syms); free(mls);
        if (err) *err = nvmtl_link_err(known);
        return nil;
    }
    NSMutableArray *tc = [NSMutableArray new];
    int rc;
    if (usesTable) {
        for (id c in nvmtl_link_table_list(lf)) {
            if (![c isKindOfClass:[NVMTLFunction class]] || !((NVMTLFunction *)c)->_air.length) {
                free(syms); free(mls);
                if (err) *err = nvmtl_link_err([NSString stringWithFormat:@"\"%@\": linked function %@ has no AIR this driver can link", fn->_fname, [c name]]);
                return nil;
            }
            [tc addObject:c];
        }
        if (!xlate_link_rt) { free(syms); free(mls); if (err) *err = nvmtl_link_err(@"this translator has no nvmtl_link_runtime - rebuild with patch_xl_vft_ffi.py"); return nil; }
        size_t nr = 0; for (NSString *s in refs) if (mods[s]) nr++;
        const char **rs = calloc(nr ? nr : 1, sizeof *rs), **rm = calloc(nr ? nr : 1, sizeof *rm);
        const char **cs = calloc(tc.count ? tc.count : 1, sizeof *cs), **cm = calloc(tc.count ? tc.count : 1, sizeof *cm);
        size_t j = 0; for (NSString *s in refs) if (mods[s]) { rs[j] = s.UTF8String; rm[j] = [mods[s] UTF8String]; j++; }
        NSMutableArray *keep = [NSMutableArray arrayWithCapacity:tc.count * 2];
        for (NSUInteger i = 0; i < tc.count; i++) { NVMTLFunction *cf = tc[i]; NSString *cn = nil; NSString *ca = nvmtl_link_candidate_air(cf, &cn);
            [keep addObject:cn ?: @""]; [keep addObject:ca ?: @""];
            cs[i] = [keep[2 * i] UTF8String]; cm[i] = [keep[2 * i + 1] UTF8String]; }
        nvlog("link: \"%s\" reads a visible function table - %lu table candidate(s), %zu direct reference(s)", fn->_fname.UTF8String, (unsigned long)tc.count, nr);
        rc = xlate_link_rt(fn->_air.UTF8String, fn->_stage.UTF8String, rs, rm, nr, cs, cm, tc.count, &out, &len, e, sizeof e);
        free(rs); free(rm); free(cs); free(cm);
    } else
    rc = xlate_link(fn->_air.UTF8String, fn->_stage.UTF8String, syms, mls, n, &out, &len, e, sizeof e);
    free(syms); free(mls);
    if (rc != 0 || !out) {
        NSString *why = [NSString stringWithFormat:@"\"%@\": %s", fn->_fname, e[0] ? e : "link failed"];
        [nvmtl_link_failures() setObject:why forKey:failKey];
        if (err) *err = nvmtl_link_err(why);
        return nil;
    }
    NSString *linked = [[NSString alloc] initWithBytes:out length:(len && out[len-1] == 0) ? len - 1 : len encoding:NSUTF8StringEncoding];
    xlate_free(out, len);
    if (!linked) { if (err) *err = nvmtl_link_err(@"linked AIR is not UTF-8"); return nil; }
    NSString *dumpDir = nvmtl_link_dump_dir();
    if (dumpDir) {
        NSString *dir = dumpDir;
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
        [fn->_air writeToFile:[dir stringByAppendingFormat:@"/%@.kernel.ll", fn->_fname] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        [linked writeToFile:[dir stringByAppendingFormat:@"/%@.linked.ll", fn->_fname] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        for (NSString *nm in mods) [mods[nm] writeToFile:[dir stringByAppendingFormat:@"/mod.%@.ll", nm] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        nvlog("link: dumped to %s", dir.UTF8String);
    }
    size_t nfc = fc.count, k2 = 0;
    uint32_t *idx = calloc(nfc ? nfc : 1, sizeof *idx), *sz = calloc(nfc ? nfc : 1, sizeof *sz);
    uint8_t (*bytes)[16] = calloc(nfc ? nfc : 1, 16); const uint8_t **pay = calloc(nfc ? nfc : 1, sizeof *pay);
    if (!idx || !sz || !bytes || !pay) { free(idx); free(sz); free(bytes); free(pay); if (err) *err = nvmtl_link_err(@"constant table allocation failed"); return nil; }
    for (NSString *ki in fc) {
        NSArray *a = fc[ki];
        if (![a isKindOfClass:[NSArray class]] || a.count == 0 || a.count > 16) {
            free(idx); free(sz); free(bytes); free(pay);
            if (err) *err = nvmtl_link_err([NSString stringWithFormat:@"constant %@ cannot be carried into the linked kernel", ki]);
            return nil;
        }
        idx[k2] = (uint32_t)ki.integerValue; sz[k2] = (uint32_t)a.count;
        for (NSUInteger j = 0; j < a.count; j++) bytes[k2][j] = (uint8_t)[a[j] unsignedCharValue];
        pay[k2] = bytes[k2]; k2++;
    }
    if (dumpDir) {
        NSMutableString *t = [NSMutableString stringWithFormat:@"stage %@\n", fn->_stage ?: @""];
        for (size_t q = 0; q < k2; q++) {
            [t appendFormat:@"%u %u", idx[q], sz[q]];
            for (uint32_t j = 0; j < sz[q]; j++) [t appendFormat:@" %02x", pay[q][j]];
            [t appendString:@"\n"];
        }
        [t writeToFile:[dumpDir stringByAppendingFormat:@"/%@.fc.txt", fn->_fname] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    }
    // Machine code compiled ahead of time for exactly this link (kernel AIR, chosen bodies, constants) runs
    // instead of a translation: the kernels that have it are the ones the translator cannot express or
    // expresses slowly, and asking it first cost minutes on MPS's depthwise convolution.
    {
        id sass = nvmtl_sass_kernel_for_linked(fn, mods, fc);
        if (sass) {
            free(idx); free(sz); free(bytes); free(pay);
            NVMTLFunction *mf = [NVMTLFunction new];
            mf->_fname = fn->_fname; mf->_stage = fn->_stage; mf->_air = linked; mf->_fc = [fc copy];
            mf->_lib = fn->_lib; mf->_fcv = fn->_fcv; mf->_specName = fn->_specName; mf->_sassKernel = sass;
            nvlog("link: \"%s\" runs from its compiled machine code; no translation", fn->_fname.UTF8String);
            return mf;
        }
    }
    NSString *te = nil;
    NSData *spv = nvmtl_translate_link_cached(linked, fn->_stage, idx, sz, pay, k2, &te);
    free(idx); free(sz); free(bytes); free(pay);
    if (!spv && dumpDir)
        [(te ?: @"(no reason)") writeToFile:[dumpDir stringByAppendingFormat:@"/%@.err.txt", fn->_fname] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    if (!spv) {
        NSString *why = [NSString stringWithFormat:@"\"%@\" linked (%lu B AIR) but did not translate: %@", fn->_fname, (unsigned long)linked.length, te];
        [nvmtl_link_failures() setObject:why forKey:failKey];
        if (err) *err = nvmtl_link_err(why);
        return nil;
    }
    if (dumpDir)
        [spv writeToFile:[dumpDir stringByAppendingFormat:@"/%@.spv", fn->_fname] atomically:YES];
    nvlog("link: \"%s\" linked %lu -> %lu B AIR, %zu constant(s) -> %lu B SPIR-V", fn->_fname.UTF8String,
          (unsigned long)fn->_air.length, (unsigned long)linked.length, k2, (unsigned long)spv.length);
    NVMTLFunction *nf = [NVMTLFunction new];
    nf->_spirv = spv; nf->_fname = fn->_fname; nf->_stage = fn->_stage; nf->_air = linked; nf->_fc = [fc copy];
    nf->_lib = fn->_lib; nf->_fcv = fn->_fcv; nf->_specName = fn->_specName;
    // THE HANDLE API IS PIPELINE-LEVEL, NOT TABLE-LEVEL. functionHandleWithFunction: must
    //    answer for ANY function in the descriptor's linkedFunctions, whether or not the entry's own AIR
    //    reads a visible function table. MEASURED: `rgs_reference_wavefront_trace` never appears in a
    //    "reads a visible function table" line - it links 24 hit shaders but reads no table itself - so
    //    usesTable was NO, nothing was recorded, and all 8 handle requests for ahs_main / chs_main_0..5
    //    came back nil with "(0 linked)". The game then binds no table and sets no acceleration
    //    structure, which is where ray tracing stopped.
    // The candidates are collected here WITHOUT the usesTable branch's validation, which must stay
    //    an error only on the table path: a function with no AIR cannot be dispatched from a table, but
    //    it must not newly refuse a pipeline that never dispatches one.
    if (!tc.count) {
        [tc addObjectsFromArray:nvmtl_link_handle_list(lf)];
        if (tc.count)
            nvlog("link: \"%s\" reads no table but links %lu function(s) - recorded so their handles resolve",
                  nf->_fname.UTF8String, (unsigned long)tc.count);
    }
    if (tc.count) {
        objc_setAssociatedObject(nf, &kNVMTLTableFns, [tc copy], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        // say WHICH function object carries the candidate list, so a pipeline that later
        //    reports "(0 linked)" can be matched against it by name and object identity.
        nvlog("link: \"%s\" carries %lu table candidate(s) on function %p",
              nf->_fname.UTF8String, (unsigned long)tc.count, (__bridge void *)nf);
    }
    return nf;
}

#pragma mark - batch 39: function pointers (compute)
@interface NVMTLFunctionHandle : NSObject <MTLFunctionHandle> { @public uint64_t _id; id<MTLFunction> _fn; } @end
@implementation NVMTLFunctionHandle
- (MTLFunctionType)functionType { return _fn.functionType; }
- (NSString *)name { return _fn.name; }
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (MTLResourceID)gpuResourceID { MTLResourceID r; r._impl = _id; return r; }
@end
@interface NVMTLVisibleFunctionTable : NSObject <MTLVisibleFunctionTable> { @public id<MTLBuffer> _buf; NSUInteger _n; } @end
@implementation NVMTLVisibleFunctionTable
- (void)setFunction:(id<MTLFunctionHandle>)f atIndex:(NSUInteger)i {
    { static _Atomic unsigned n; if (atomic_fetch_add(&n, 1) < 8) { FILE *lg = fopen(nvmtl_log_path(), "a"); if (lg) { fprintf(lg, "pid %d NVMTL: RTBIND visible table setFunction slot %lu <- %s id %llu\n", getpid(), (unsigned long)i, f ? [[f name] UTF8String] : "nil", f && [(id)f isKindOfClass:[NVMTLFunctionHandle class]] ? (unsigned long long)((NVMTLFunctionHandle *)f)->_id : 0ull); fclose(lg); } } }
    if (i >= _n) { nvlog("visible function table: setFunction atIndex:%lu beyond %lu slots - IGNORED", (unsigned long)i, (unsigned long)_n); return; }
    if (f && ![(id)f isKindOfClass:[NVMTLFunctionHandle class]]) { nvlog("visible function table: foreign handle %s - slot %lu set NULL", NSStringFromClass([(id)f class]).UTF8String, (unsigned long)i); f = nil; }
    ((uint64_t *)_buf.contents)[i] = f ? ((NVMTLFunctionHandle *)f)->_id : 0;
}
- (void)setFunctions:(const id<MTLFunctionHandle> [])fs withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setFunction:fs[k] atIndex:r.location + k];
}
- (MTLResourceID)gpuResourceID { MTLResourceID r; r._impl = _buf.gpuAddress; return r; }
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (NSString *)label { return _buf.label; }
- (void)setLabel:(NSString *)l { _buf.label = l; }
- (MTLStorageMode)storageMode { return _buf.storageMode; }
- (MTLCPUCacheMode)cpuCacheMode { return _buf.cpuCacheMode; }
- (MTLHazardTrackingMode)hazardTrackingMode { return _buf.hazardTrackingMode; }
- (MTLResourceOptions)resourceOptions { return _buf.resourceOptions; }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable {}
- (NSUInteger)allocatedSize { return _buf.allocatedSize; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)s { return MTLPurgeableStateNonVolatile; }
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)t { return nvmtl_owner_check(t); }
- (id)forwardingTargetForSelector:(SEL)s { return _buf; }
@end

#define NVMTL_IFT_BUFFERS 31
#define NVMTL_IFT_OPAQUE 0x8000000000000001ull
@interface NVMTLIntersectionFunctionTable : NSObject <MTLIntersectionFunctionTable> { @public id<MTLBuffer> _buf; NSUInteger _n; } @end
@implementation NVMTLIntersectionFunctionTable
- (uint64_t *)nvmtlWords { return (uint64_t *)_buf.contents; }
- (void)setFunction:(id<MTLFunctionHandle>)f atIndex:(NSUInteger)i {
    if (i >= _n) { nvlog("intersection function table: setFunction atIndex:%lu beyond %lu slots - IGNORED", (unsigned long)i, (unsigned long)_n); return; }
    if (f && ![(id)f isKindOfClass:[NVMTLFunctionHandle class]]) { nvlog("intersection function table: foreign handle %s - slot %lu set NULL", NSStringFromClass([(id)f class]).UTF8String, (unsigned long)i); f = nil; }
    [self nvmtlWords][NVMTL_IFT_BUFFERS + i] = f ? ((NVMTLFunctionHandle *)f)->_id : 0;
}
- (void)setFunctions:(const id<MTLFunctionHandle> [])fs withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setFunction:fs[k] atIndex:r.location + k];
}
- (void)setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)s atIndex:(NSUInteger)i {
    if (i >= _n) { nvlog("intersection function table: opaque triangle atIndex:%lu beyond %lu slots - IGNORED", (unsigned long)i, (unsigned long)_n); return; }
    [self nvmtlWords][NVMTL_IFT_BUFFERS + i] = NVMTL_IFT_OPAQUE;
}
- (void)setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)s withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setOpaqueTriangleIntersectionFunctionWithSignature:s atIndex:r.location + k];
}
- (void)setOpaqueCurveIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)s atIndex:(NSUInteger)i {
    nvlog("intersection function table: opaque curve function at %lu - curve segments are always intersected by the built-in runtime (opaque); slot left as it was", (unsigned long)i);
}
- (void)setOpaqueCurveIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)s withRange:(NSRange)r {
    nvlog("intersection function table: opaque curve functions %lu+%lu - curve segments are always intersected by the built-in runtime (opaque)", (unsigned long)r.location, (unsigned long)r.length);
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)off atIndex:(NSUInteger)i {
    if (i >= NVMTL_IFT_BUFFERS) { nvlog("intersection function table: setBuffer atIndex:%lu beyond Metal's 31 - IGNORED", (unsigned long)i); return; }
    [self nvmtlWords][i] = b ? b.gpuAddress + off : 0;
}
- (void)setBuffers:(const id<MTLBuffer> [])bs offsets:(const NSUInteger [])offs withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setBuffer:bs[k] offset:offs[k] atIndex:r.location + k];
}
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if (t && ![(id)t isKindOfClass:[NVMTLVisibleFunctionTable class]]) { nvlog("intersection function table: foreign visible table %s - REFUSED", NSStringFromClass([(id)t class]).UTF8String); return; }
    [self setBuffer:t ? ((NVMTLVisibleFunctionTable *)t)->_buf : nil offset:0 atIndex:i];
}
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> [])ts withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setVisibleFunctionTable:ts[k] atBufferIndex:r.location + k];
}
- (MTLResourceID)gpuResourceID { MTLResourceID r; r._impl = _buf.gpuAddress; return r; }
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (NSString *)label { return _buf.label; }
- (void)setLabel:(NSString *)l { _buf.label = l; }
- (MTLStorageMode)storageMode { return _buf.storageMode; }
- (MTLCPUCacheMode)cpuCacheMode { return _buf.cpuCacheMode; }
- (MTLHazardTrackingMode)hazardTrackingMode { return _buf.hazardTrackingMode; }
- (MTLResourceOptions)resourceOptions { return _buf.resourceOptions; }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable {}
- (NSUInteger)allocatedSize { return _buf.allocatedSize; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)s { return MTLPurgeableStateNonVolatile; }
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)t { return nvmtl_owner_check(t); }
- (id)forwardingTargetForSelector:(SEL)s { return _buf; }
@end
static id<MTLIntersectionFunctionTable> nvmtl_ift_new(MTLIntersectionFunctionTableDescriptor *d, const char *what) {
    const NSUInteger n = d.functionCount ? d.functionCount : 1, bytes = (NVMTL_IFT_BUFFERS + n) * sizeof(uint64_t);
    id<MTLBuffer> b = [(id<MTLDevice>)gNVMTLMainDevice newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (!b) { nvlog("%s: intersection function table of %lu slot(s) - buffer allocation FAILED -> nil", what, (unsigned long)n); return nil; }
    memset(b.contents, 0, bytes);
    NVMTLIntersectionFunctionTable *t = [NVMTLIntersectionFunctionTable new]; t->_buf = b; t->_n = n;
    nvlog("%s: intersection function table, %lu slot(s) -> %p", what, (unsigned long)n, t);
    return t;
}
@implementation NVMTLComputePipelineState (RT3IntersectionTables)
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor *)d {
    return nvmtl_ift_new(d, "newIntersectionFunctionTableWithDescriptor");
}
@end
@implementation NVMTLComputeCommandEncoder (RT3IntersectionTables)
- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if (t && ![(id)t isKindOfClass:[NVMTLIntersectionFunctionTable class]]) { nvlog("setIntersectionFunctionTable: foreign %s - REFUSED", NSStringFromClass([(id)t class]).UTF8String); return; }
    [self setBuffer:t ? ((NVMTLIntersectionFunctionTable *)t)->_buf : nil offset:0 atIndex:i];
}
- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> [])ts withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setIntersectionFunctionTable:ts[k] atBufferIndex:r.location + k];
}
@end
// ---- a pipeline's own handle list, binary functions, functions added later ----------------------------------------
// A pipeline answers functionHandleWithFunction: from the list of its stage (nvmtl_link_handle_list of the descriptor's
// linked functions), kept on the pipeline. The list on the linked function object stays for the paths that only see the
// function, but a kernel that needs no link IS the caller's function object, shared by every pipeline made from it with
// other linked functions, so the first pipeline's list would have answered for all of them.
// The binary functions are kept apart as well: functionHandleWithName: names only functions linked at the IR level
// (Apple's M1 answers nil for a binary function's name), and the descriptor is kept for
// newComputePipelineStateWithAdditionalBinaryFunctions:, which builds the pipeline again with the added functions at the
// end of the list - the switch the link builds over the list then has a case for each, and every earlier function keeps
// its handle.
static char kNVMTLPipeFns[3], kNVMTLPipeBinFns[3], kNVMTLPipeDesc;
enum { NVMTL_PIPE_COMPUTE = 0, NVMTL_PIPE_VERTEX = 1, NVMTL_PIPE_FRAGMENT = 2 };
void nvmtl_pipe_keep_handles(id ps, int stage, MTLLinkedFunctions *lf)
{
    if (!ps || stage < 0 || stage > 2) return;
    NSArray *c = nvmtl_link_handle_list(lf);
    objc_setAssociatedObject(ps, &kNVMTLPipeFns[stage], c.count ? c : nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(ps, &kNVMTLPipeBinFns[stage], lf.binaryFunctions.count ? [lf.binaryFunctions copy] : nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
void nvmtl_pipe_keep_descriptor(id ps, id d) { if (ps && d) objc_setAssociatedObject(ps, &kNVMTLPipeDesc, [d copy], OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
static NSArray *nvmtl_pipe_handles(id ps, int stage, id linkedFn)
{
    NSArray *c = objc_getAssociatedObject(ps, &kNVMTLPipeFns[stage]);
    return c ?: (linkedFn ? objc_getAssociatedObject(linkedFn, &kNVMTLTableFns) : nil);
}
// The handle of the function named `name` among the IR-linked functions of a stage (not its binary functions).
static id<MTLFunctionHandle> nvmtl_pipe_handle_named(id ps, int stage, id linkedFn, NSString *name)
{
    NSArray *c = nvmtl_pipe_handles(ps, stage, linkedFn), *bin = objc_getAssociatedObject(ps, &kNVMTLPipeBinFns[stage]);
    for (NSUInteger k = 0; k < c.count; k++) {
        NVMTLFunction *cf = c[k];
        if ([bin containsObject:cf] || !([cf.name isEqualToString:name] || [cf->_fname isEqualToString:name])) continue;
        NVMTLFunctionHandle *h = [NVMTLFunctionHandle new]; h->_id = k + 1; h->_fn = cf; return h;
    }
    nvlog("functionHandleWithName: \"%s\" is not a function this pipeline links at the IR level (%lu linked) -> nil", name.UTF8String, (unsigned long)c.count);
    return nil;
}
// `fns` added to the binary functions of `lf` (a copy), after the ones it has; NO with *err when one is not this driver's.
static MTLLinkedFunctions *nvmtl_lf_with_binaries(MTLLinkedFunctions *lf, NSArray *fns, NSError **err)
{
    MTLLinkedFunctions *n = lf ? [lf copy] : [MTLLinkedFunctions linkedFunctions];
    NSMutableArray *bin = [NSMutableArray arrayWithArray:n.binaryFunctions ?: @[]];
    for (id f in fns) {
        if (![f isKindOfClass:[NVMTLFunction class]] || !((NVMTLFunction *)f)->_air.length) {
            if (err) *err = nvmtl_link_err([NSString stringWithFormat:@"additional binary function %@ (%s) has no AIR this driver can link",
                                            [f respondsToSelector:@selector(name)] ? [f name] : @"?", object_getClassName(f)]);
            return nil;
        }
        if (![bin containsObject:f]) [bin addObject:f];
    }
    n.binaryFunctions = bin;
    return n;
}
@implementation NVMTLComputePipelineState (B39FunctionPointers)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"   // a protocol method, new here (the others predate)
- (id<MTLComputePipelineState>)newComputePipelineStateWithAdditionalBinaryFunctions:(NSArray<id<MTLFunction>> *)fns error:(NSError **)err {
    if (err) *err = nil;
    MTLComputePipelineDescriptor *d = [objc_getAssociatedObject(self, &kNVMTLPipeDesc) copy];
    if (!d) {
        if (err) *err = nvmtl_link_err(@"newComputePipelineStateWithAdditionalBinaryFunctions: this pipeline was not made from a compute pipeline descriptor, so there is nothing to build again with the added functions");
        return nil;
    }
    MTLLinkedFunctions *lf = nvmtl_lf_with_binaries(d.linkedFunctions, fns, err);
    if (!lf) return nil;
    d.linkedFunctions = lf;
    nvlog("newComputePipelineStateWithAdditionalBinaryFunctions: \"%s\" + %lu function(s) -> %lu binary function(s)",
          d.computeFunction.name.UTF8String, (unsigned long)fns.count, (unsigned long)lf.binaryFunctions.count);
    return [(id<MTLDevice>)gNVMTLMainDevice newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:err];
}
#pragma clang diagnostic pop
- (id<MTLFunctionHandle>)nvmtlHandleNamed:(NSString *)name { return nvmtl_pipe_handle_named(self, NVMTL_PIPE_COMPUTE, _function, name); }
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)f {
    NSArray *c = nvmtl_pipe_handles(self, NVMTL_PIPE_COMPUTE, _function);
    for (NSUInteger k = 0; k < c.count; k++) {
        NVMTLFunction *cf = c[k];
        if (cf == (id)f || [cf.name isEqualToString:f.name]) {
            NVMTLFunctionHandle *h = [NVMTLFunctionHandle new]; h->_id = k + 1; h->_fn = f; return h;
        }
    }
    // THE PIPELINE'S OWN FUNCTION IS PART OF THE PIPELINE, so a handle for it is a legitimate
    //    request and nil is what stopped ray tracing. MEASURED on the game: it asks only for the ray
    //    generation shader's own handle (rgs_diffuse_main / rgs_importance_main), and on nil it goes no
    //    further - setVisibleFunctionTable, setIntersectionFunctionTable and setAccelerationStructure are
    //    called ZERO times in a whole run, so without a handle it cannot fill the table it just built and
    //    abandons RT entirely.
    // The id handed back is the table's RESERVED UNBOUND INDEX 0, not a dispatch slot: the linkage
    //    sizes the table as entries+1 with candidates at 1..N, so 0 is "no entry here" and the shader's
    //    own null test is what reads it. That is the truthful answer - the entry is not one of the table's
    //    candidates and the dispatcher has no case for it - and it lets the game proceed instead of
    //    giving up. If a frame shows the ray generation shader needed to be CALLED through this slot, the
    //    fix is to add the entry to the dispatcher, not to invent a slot number here.
    if (_function && (f == (id)_function
                      || [f.name isEqualToString:((NVMTLFunction *)_function)->_fname])) {
        NVMTLFunctionHandle *h = [NVMTLFunctionHandle new];
        h->_id = 0;
        h->_fn = f;
        nvlog("functionHandleWithFunction: \"%s\" IS this pipeline's own function - handle with the reserved unbound index 0 (%lu table candidate(s), none of them the entry)",
              f.name.UTF8String, (unsigned long)c.count);
        return h;
    }
    // "(0 linked)" alone cannot distinguish "the candidate list was never attached" from "this pipeline
    // was built from a different function object than the one it was attached to", so name both.
    nvlog("functionHandleWithFunction: \"%s\" is not a linked function of this pipeline (%lu linked, pipeline function \"%s\") -> nil",
          f.name.UTF8String, (unsigned long)c.count,
          _function ? ((NVMTLFunction *)_function)->_fname.UTF8String : "(none)");
    return nil;
}
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)d {
    NSUInteger n = d.functionCount ? d.functionCount : 1;
    id<MTLBuffer> b = [(id<MTLDevice>)gNVMTLMainDevice newBufferWithLength:n * sizeof(uint64_t) options:MTLResourceStorageModeShared];
    if (!b) { nvlog("newVisibleFunctionTableWithDescriptor: %lu slots - buffer allocation FAILED -> nil", (unsigned long)n); return nil; }
    memset(b.contents, 0, n * sizeof(uint64_t));
    NVMTLVisibleFunctionTable *t = [NVMTLVisibleFunctionTable new]; t->_buf = b; t->_n = n;
    nvlog("newVisibleFunctionTableWithDescriptor: %lu slot(s) -> %p", (unsigned long)n, t);
    return t;
}
@end
@implementation NVMTLComputeCommandEncoder (B39FunctionPointers)
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if (t && ![(id)t isKindOfClass:[NVMTLVisibleFunctionTable class]]) { nvlog("setVisibleFunctionTable: foreign %s - REFUSED", NSStringFromClass([(id)t class]).UTF8String); return; }
    { static _Atomic unsigned n; if (atomic_fetch_add(&n, 1) < 4) { FILE *lg = fopen(nvmtl_log_path(), "a"); if (lg) { fprintf(lg, "pid %d NVMTL: RTBIND setVisibleFunctionTable index %lu %s\n", getpid(), (unsigned long)i, t ? "table" : "nil"); fclose(lg); } } }
    [self setBuffer:t ? ((NVMTLVisibleFunctionTable *)t)->_buf : nil offset:0 atIndex:i];
}
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> [])ts withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setVisibleFunctionTable:ts[k] atBufferIndex:r.location + k];
}
@end

#pragma mark - batch 41: dynamic libraries (objects), render-stage function tables, binary archives
@implementation NVMTLDynamicLibrary
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (NSString *)installName { return _installName; }
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)err {
    NSError *we = nil;
    if (_lib->_raw.length && [_lib->_raw writeToURL:url options:NSDataWritingAtomic error:&we]) return YES;
    if (err) *err = nvmtl_b41_err([NSString stringWithFormat:@"dylib serializeToURL: %@ - %@", url.path, we.localizedDescription ?: @"no container bytes"]);
    return NO;
}
- (dispatch_data_t)airData {
    NSData *r = _lib->_raw;
    return dispatch_data_create(r.bytes, r.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
}
@end

id nvmtl_new_dylib(id<MTLLibrary> lib, NSError **err) {
    if (err) *err = nil;
    if (![(id)lib isKindOfClass:[NVMTLLibrary class]]) {
        if (err) *err = nvmtl_b41_err([NSString stringWithFormat:@"newDynamicLibrary: %s is not a library this driver made", object_getClassName(lib)]);
        return nil;
    }
    NVMTLLibrary *l = (NVMTLLibrary *)lib; const uint8_t *b = l->_raw.bytes;
    if (l->_raw.length < 0x58 || memcmp(b, "MTLB", 4) != 0 || b[0x0A] != 2) {
        if (err) *err = nvmtl_b41_err([NSString stringWithFormat:@"newDynamicLibrary: not a dynamic library (MTLB file type %d) - compile it with MTLLibraryTypeDynamic",
                                       l->_raw.length > 0x0A ? (int)b[0x0A] : -1]);
        return nil;
    }
    NSString *nm = nil; nvmtl_mtlb_dynamic(l->_raw, &nm, NULL);
    NVMTLDynamicLibrary *d = [NVMTLDynamicLibrary new]; d->_lib = l; d->_installName = nm;
    nvlog("newDynamicLibrary: %lu exported function(s), installName %s", (unsigned long)l->_airs.count, nm.UTF8String ?: "(none)");
    return d;
}
id nvmtl_new_dylib_url(NSURL *url, NSError **err) {
    id<MTLLibrary> l = [(id<MTLDevice>)gNVMTLMainDevice newLibraryWithURL:url error:err];
    return l ? nvmtl_new_dylib(l, err) : nil;
}
void nvmtl_note_compile_dylibs(id lib, NSArray *libraries) {
    if (![lib isKindOfClass:[NVMTLLibrary class]] || !libraries.count) return;
    objc_setAssociatedObject(lib, &kNVMTLCompileDylibs, [libraries copy], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

void nvmtl_render_keep_linked(id ps, id<MTLFunction> vf, id<MTLFunction> ff) {
    if (vf && objc_getAssociatedObject(vf, &kNVMTLTableFns)) objc_setAssociatedObject(ps, &kNVMTLRenderVert, vf, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (ff && objc_getAssociatedObject(ff, &kNVMTLTableFns)) objc_setAssociatedObject(ps, &kNVMTLRenderFrag, ff, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static id<MTLVisibleFunctionTable> nvmtl_vft_new(MTLVisibleFunctionTableDescriptor *d, const char *what) {
    NSUInteger n = d.functionCount ? d.functionCount : 1;
    id<MTLBuffer> b = [(id<MTLDevice>)gNVMTLMainDevice newBufferWithLength:n * sizeof(uint64_t) options:MTLResourceStorageModeShared];
    if (!b) { nvlog("%s newVisibleFunctionTableWithDescriptor: %lu slots - buffer allocation FAILED -> nil", what, (unsigned long)n); return nil; }
    memset(b.contents, 0, n * sizeof(uint64_t));
    NVMTLVisibleFunctionTable *t = [NVMTLVisibleFunctionTable new]; t->_buf = b; t->_n = n;
    nvlog("%s newVisibleFunctionTableWithDescriptor: %lu slot(s) -> %p", what, (unsigned long)n, t);
    return t;
}
@implementation NVMTLRenderPipelineState (B41FunctionPointers)
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)f stage:(MTLRenderStages)st {
    const void *key = (st & MTLRenderStageFragment) ? &kNVMTLRenderFrag : (st & MTLRenderStageVertex) ? &kNVMTLRenderVert : NULL;
    id lf = key ? objc_getAssociatedObject(self, key) : nil;
    NSArray *c = key ? nvmtl_pipe_handles(self, (st & MTLRenderStageFragment) ? NVMTL_PIPE_FRAGMENT : NVMTL_PIPE_VERTEX, lf) : nil;
    for (NSUInteger k = 0; k < c.count; k++) {
        NVMTLFunction *cf = c[k];
        if (cf == (id)f || [cf.name isEqualToString:f.name]) {
            NVMTLFunctionHandle *h = [NVMTLFunctionHandle new]; h->_id = k + 1; h->_fn = f; return h;
        }
    }
    nvlog("render functionHandleWithFunction: \"%s\" stage %lu is not a linked function of this pipeline (%lu linked) -> nil",
          f.name.UTF8String, (unsigned long)st, (unsigned long)c.count);
    return nil;
}
- (id<MTLFunctionHandle>)nvmtlHandleNamed:(NSString *)name stage:(MTLRenderStages)st {
    if (!(st & (MTLRenderStageVertex | MTLRenderStageFragment))) {
        nvlog("functionHandleWithName: \"%s\" stage %lu: only vertex and fragment functions link here -> nil", name.UTF8String, (unsigned long)st); return nil; }
    const BOOL frag = (st & MTLRenderStageFragment) != 0;
    return nvmtl_pipe_handle_named(self, frag ? NVMTL_PIPE_FRAGMENT : NVMTL_PIPE_VERTEX, objc_getAssociatedObject(self, frag ? &kNVMTLRenderFrag : &kNVMTLRenderVert), name);
}
// The pipeline built again from its descriptor with the added binary functions after the ones each stage has. Tile
// functions do not exist here (no tile pipelines on this driver), so tile additions are refused, not dropped.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"   // a protocol method, new here (the others predate)
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithAdditionalBinaryFunctions:(MTLRenderPipelineFunctionsDescriptor *)add error:(NSError **)err {
    if (err) *err = nil;
    if (add.tileAdditionalBinaryFunctions.count) {
        if (err) *err = nvmtl_link_err(@"newRenderPipelineStateWithAdditionalBinaryFunctions: tile functions are not supported by this driver (no tile pipelines), so tileAdditionalBinaryFunctions cannot be added");
        return nil;
    }
    MTLRenderPipelineDescriptor *d = [_descriptor copy];
    if (!d) { if (err) *err = nvmtl_link_err(@"newRenderPipelineStateWithAdditionalBinaryFunctions: this pipeline has no descriptor to build again"); return nil; }
    if (add.vertexAdditionalBinaryFunctions.count) {
        MTLLinkedFunctions *v = nvmtl_lf_with_binaries(d.vertexLinkedFunctions, add.vertexAdditionalBinaryFunctions, err); if (!v) return nil;
        d.vertexLinkedFunctions = v;
    }
    if (add.fragmentAdditionalBinaryFunctions.count) {
        MTLLinkedFunctions *f = nvmtl_lf_with_binaries(d.fragmentLinkedFunctions, add.fragmentAdditionalBinaryFunctions, err); if (!f) return nil;
        d.fragmentLinkedFunctions = f;
    }
    nvlog("newRenderPipelineStateWithAdditionalBinaryFunctions: \"%s\"/\"%s\" + %lu vertex, %lu fragment function(s)", d.vertexFunction.name.UTF8String,
          d.fragmentFunction.name.UTF8String ?: "(none)", (unsigned long)add.vertexAdditionalBinaryFunctions.count, (unsigned long)add.fragmentAdditionalBinaryFunctions.count);
    return [(id<MTLDevice>)gNVMTLMainDevice newRenderPipelineStateWithDescriptor:d error:err];
}
#pragma clang diagnostic pop
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)d stage:(MTLRenderStages)st {
    return nvmtl_vft_new(d, (st & MTLRenderStageFragment) ? "fragment" : "vertex");
}
@end
@implementation NVMTLRenderCommandEncoder (B41FunctionPointers)
- (void)setFragmentVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if (t && ![(id)t isKindOfClass:[NVMTLVisibleFunctionTable class]]) { nvlog("setFragmentVisibleFunctionTable: foreign %s - REFUSED", NSStringFromClass([(id)t class]).UTF8String); return; }
    [self setFragmentBuffer:t ? ((NVMTLVisibleFunctionTable *)t)->_buf : nil offset:0 atIndex:i];
}
- (void)setFragmentVisibleFunctionTables:(const id<MTLVisibleFunctionTable> [])ts withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setFragmentVisibleFunctionTable:ts[k] atBufferIndex:r.location + k];
}
- (void)setVertexVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if (t && ![(id)t isKindOfClass:[NVMTLVisibleFunctionTable class]]) { nvlog("setVertexVisibleFunctionTable: foreign %s - REFUSED", NSStringFromClass([(id)t class]).UTF8String); return; }
    [self setVertexBuffer:t ? ((NVMTLVisibleFunctionTable *)t)->_buf : nil offset:0 atIndex:i];
}
- (void)setVertexVisibleFunctionTables:(const id<MTLVisibleFunctionTable> [])ts withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setVertexVisibleFunctionTable:ts[k] atBufferIndex:r.location + k];
}
@end

@implementation NVMTLRenderPipelineState (RT3IntersectionTables)
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor *)d stage:(MTLRenderStages)st {
    return nvmtl_ift_new(d, "render newIntersectionFunctionTableWithDescriptor");
}
@end
@implementation NVMTLRenderCommandEncoder (RT3IntersectionTables)
- (void)setVertexIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if (t && ![(id)t isKindOfClass:[NVMTLIntersectionFunctionTable class]]) { nvlog("setVertexIntersectionFunctionTable: foreign %s - REFUSED", NSStringFromClass([(id)t class]).UTF8String); return; }
    [self setVertexBuffer:t ? ((NVMTLIntersectionFunctionTable *)t)->_buf : nil offset:0 atIndex:i];
}
- (void)setFragmentIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if (t && ![(id)t isKindOfClass:[NVMTLIntersectionFunctionTable class]]) { nvlog("setFragmentIntersectionFunctionTable: foreign %s - REFUSED", NSStringFromClass([(id)t class]).UTF8String); return; }
    [self setFragmentBuffer:t ? ((NVMTLIntersectionFunctionTable *)t)->_buf : nil offset:0 atIndex:i];
}
- (void)setVertexIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> [])ts withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setVertexIntersectionFunctionTable:ts[k] atBufferIndex:r.location + k];
}
- (void)setFragmentIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> [])ts withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setFragmentIntersectionFunctionTable:ts[k] atBufferIndex:r.location + k];
}
@end
@interface NVMTLBinaryArchive : NSObject <MTLBinaryArchive> { @public NSMutableSet<NSString *> *_keys; NSString *_label; } @end
static uint64_t nvmtl_fnv64(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (; s && *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}
static NSString *nvmtl_fn_key(id<MTLFunction> f) {
    if (![(id)f isKindOfClass:[NVMTLFunction class]]) return nil;
    NVMTLFunction *fn = (NVMTLFunction *)f;
    NSData *fc = fn->_fc.count ? [NSJSONSerialization dataWithJSONObject:fn->_fc options:NSJSONWritingSortedKeys error:NULL] : nil;
    NSString *fcs = fc ? [[NSString alloc] initWithData:fc encoding:NSUTF8StringEncoding] : @"";
    return [NSString stringWithFormat:@"%@#%016llx#%016llx", fn->_fname, nvmtl_fnv64(fn->_air.UTF8String), nvmtl_fnv64(fcs.UTF8String)];
}
NSString *nvmtl_archive_key_compute(id<MTLFunction> f) {
    NSString *k = nvmtl_fn_key(f); return k ? [@"compute:" stringByAppendingString:k] : nil;
}
NSString *nvmtl_archive_key_render(MTLRenderPipelineDescriptor *d) {
    NSString *v = nvmtl_fn_key(d.vertexFunction), *f = nvmtl_fn_key(d.fragmentFunction);
    return (v && f) ? [NSString stringWithFormat:@"render:%@|%@", v, f] : nil;
}
@implementation NVMTLBinaryArchive
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (BOOL)nvmtlAdd:(NSString *)key what:(const char *)what error:(NSError **)err {
    if (!key) { if (err) *err = nvmtl_b41_err([NSString stringWithFormat:@"binary archive: %s names a function this driver did not make - not added", what]); return NO; }
    @synchronized (self) { [_keys addObject:key]; }
    nvlog("binary archive %p: added %s", self, key.UTF8String);
    return YES;
}
- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)err {
    return [self nvmtlAdd:nvmtl_archive_key_compute(d.computeFunction) what:"addComputePipelineFunctions" error:err];
}
- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)err {
    return [self nvmtlAdd:nvmtl_archive_key_render(d) what:"addRenderPipelineFunctions" error:err];
}
- (BOOL)addTileRenderPipelineFunctionsWithDescriptor:(MTLTileRenderPipelineDescriptor *)d error:(NSError **)err {
    if (err) *err = nvmtl_b41_err(@"binary archive: tile render pipelines are not implemented by this driver - not added");
    return NO;
}
- (BOOL)addMeshRenderPipelineFunctionsWithDescriptor:(MTLMeshRenderPipelineDescriptor *)d error:(NSError **)err {
    if (err) *err = nvmtl_b41_err(@"binary archive: mesh render pipelines are not implemented by this driver yet - not added");
    return NO;
}
- (BOOL)addLibraryWithDescriptor:(MTLStitchedLibraryDescriptor *)d error:(NSError **)err {
    nvlog("binary archive %p: addLibraryWithDescriptor - a stitched library compiles on demand here; nothing to record", self);
    return YES;
}
- (BOOL)addFunctionWithDescriptor:(MTLFunctionDescriptor *)d library:(id<MTLLibrary>)lib error:(NSError **)err {
    return [self nvmtlAdd:[@"function:" stringByAppendingString:d.name ?: @"(nil)"] what:"addFunctionWithDescriptor" error:err];
}
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)err {
    NSArray *keys; @synchronized (self) { keys = [_keys.allObjects sortedArrayUsingSelector:@selector(compare:)]; }
    NSError *pe = nil;
    NSData *pl = [NSPropertyListSerialization dataWithPropertyList:@{@"format": @"NVMTLBinaryArchive", @"version": @1, @"keys": keys}
                                                            format:NSPropertyListBinaryFormat_v1_0 options:0 error:&pe];
    if (pl && [pl writeToURL:url options:NSDataWritingAtomic error:&pe]) { nvlog("binary archive %p: serialized %lu key(s) to %s", self, (unsigned long)keys.count, url.path.UTF8String); return YES; }
    if (err) *err = nvmtl_b41_err([NSString stringWithFormat:@"binary archive serializeToURL: %@ - %@", url.path, pe.localizedDescription ?: @"?"]);
    return NO;
}
@end
id nvmtl_new_binary_archive(MTLBinaryArchiveDescriptor *d, NSError **err) {
    if (err) *err = nil;
    NVMTLBinaryArchive *a = [NVMTLBinaryArchive new]; a->_keys = [NSMutableSet new];
    if (d.url) {
        NSData *data = [NSData dataWithContentsOfURL:d.url];
        if (!data) { if (err) *err = nvmtl_b41_err([NSString stringWithFormat:@"newBinaryArchiveWithDescriptor: cannot read %@", d.url.path ?: d.url.absoluteString]); return nil; }
        NSDictionary *pl = [NSPropertyListSerialization propertyListWithData:data options:0 format:NULL error:NULL];
        if ([pl isKindOfClass:[NSDictionary class]] && [pl[@"format"] isEqual:@"NVMTLBinaryArchive"] && [pl[@"keys"] isKindOfClass:[NSArray class]]) {
            for (id k in pl[@"keys"]) if ([k isKindOfClass:[NSString class]]) [a->_keys addObject:k];
        } else
            nvlog("newBinaryArchiveWithDescriptor: %s is not an archive this driver wrote (%lu B) - opened EMPTY, every lookup misses",
                  d.url.path.UTF8String, (unsigned long)data.length);
    }
    nvlog("newBinaryArchiveWithDescriptor: %s -> %p, %lu key(s)", d.url ? d.url.path.UTF8String : "(new)", a, (unsigned long)a->_keys.count);
    return a;
}
NSString *nvmtl_archive_refusal(NSArray *archives, NSString *key) {
    if (!archives.count) return nil;
    for (id a in archives)
        if (![a isKindOfClass:[NVMTLBinaryArchive class]]) {
            nvlog("binary archive: fail-on-miss against a foreign %s - NOT enforced", NSStringFromClass([a class]).UTF8String);
            return nil;
        }
    if (!key) return nil;
    for (NVMTLBinaryArchive *a in archives) { @synchronized (a) { if ([a->_keys containsObject:key]) return nil; } }
    return [NSString stringWithFormat:@"Unable to find %@ in binary archives (MTLPipelineOptionFailOnBinaryArchiveMiss, %lu archive(s) searched)",
            key, (unsigned long)archives.count];
}

#pragma mark - batch 42a: mesh shaders (compute emulation - NVK has no VK_EXT_mesh_shader)
static NSArray *nvmtl_lowered_stage_arguments(NVMTLComputePipelineState *kp, NVMTLFunction *stageFn) {
    if (!kp || !kp->_function || !stageFn) return @[];
    NSDictionary *names = nvmtl_air_names(stageFn);
    NSMutableArray *out = [NSMutableArray new];
    for (NVMTLArgument *a in nvmtl_arguments_for_function(kp->_function)) {
        if (a->_type == MTLArgumentTypeBuffer && (a->_index == 29 || a->_index == 30)) continue;
        if (a->_type == (NSUInteger)MTLBindingTypeThreadgroupMemory) continue;   // the lowered kernel's, not the stage's
        NSString *k = [NSString stringWithFormat:@"%@:%lu", a->_type == MTLArgumentTypeBuffer ? @"buffer" : a->_type == MTLArgumentTypeTexture ? @"texture" : @"sampler", (unsigned long)a->_index];
        NSString *n = names[@"args"][k]; if ([n isKindOfClass:[NSString class]]) a->_name = n;
        [out addObject:a];
    }
    return out;
}
@interface NVMTLMeshInfo : NSObject { @public NVMTLComputePipelineState *_k, *_o, *_ki; NVMTLRenderPipelineState *_psi;
    uint32_t _lay[9], _layi[9], _cap, _capi; NSString *_name, *_indWhy;
    NSUInteger _tgMesh, _tgObj, _gridAsk;
} @end
@implementation NVMTLMeshInfo @end
static char kNVMTLMeshInfo;
static NSUInteger nvmtl_mesh_stage_max(NVMTLMeshInfo *mi, NVMTLComputePipelineState *k, NSUInteger rec, const char *stage) {
    if (rec) return rec;
    if (k) return [k maxTotalThreadsPerThreadgroup];
    uint32_t inv = 0; nvmtl_vk_limits(NULL, NULL, NULL, &inv, NULL);
    NSUInteger v = inv ?: nvmtl_compute_maxt();
    nvlog("mesh pipeline \"%s\": the %s stage's lowered kernel is out of reach - its limit answers the device's %lu", mi->_name.UTF8String, stage, (unsigned long)v);
    return v;
}
@implementation NVMTLRenderPipelineState (AppsPSOMesh)
- (NSUInteger)objectThreadExecutionWidth { NVMTLMeshInfo *mi = objc_getAssociatedObject(self, &kNVMTLMeshInfo);
    return mi && mi->_o ? [mi->_o threadExecutionWidth] : [self threadExecutionWidth]; }
- (NSUInteger)meshThreadExecutionWidth { NVMTLMeshInfo *mi = objc_getAssociatedObject(self, &kNVMTLMeshInfo);
    return mi && mi->_k ? [mi->_k threadExecutionWidth] : [self threadExecutionWidth]; }
- (NSUInteger)maxTotalThreadsPerObjectThreadgroup { NVMTLMeshInfo *mi = objc_getAssociatedObject(self, &kNVMTLMeshInfo);
    return mi && mi->_o ? nvmtl_mesh_stage_max(mi, mi->_o, mi->_tgObj, "object") : 0; }
- (NSUInteger)maxTotalThreadsPerMeshThreadgroup { NVMTLMeshInfo *mi = objc_getAssociatedObject(self, &kNVMTLMeshInfo);
    return mi ? nvmtl_mesh_stage_max(mi, mi->_k, mi->_tgMesh, "mesh") : 0; }
- (NSUInteger)maxTotalThreadgroupsPerMeshGrid { NVMTLMeshInfo *mi = objc_getAssociatedObject(self, &kNVMTLMeshInfo);
    return !mi ? 0 : mi->_o ? mi->_cap : (mi->_gridAsk ?: 1024); }
@end
@interface NVMTLMeshBinds : NSObject { @public NSMutableDictionary *_obj, *_mesh; } @end
@implementation NVMTLMeshBinds @end
static char kNVMTLMeshBinds;

static id nvmtl_mesh_fail(NSError **err, NSString *why) {
    nvlog("mesh pipeline: %s", why.UTF8String);
    if (err) *err = [NSError errorWithDomain:@"NVMTL" code:42 userInfo:@{NSLocalizedDescriptionKey: why}];
    return nil;
}

static NSData *nvmtl_mesh_translate(NSString *ll, NSString *stage, NSString **why) {
    uint32_t idx0 = 0, sz0 = 0; const uint8_t *pay0 = NULL;
    return nvmtl_translate_with_fc(ll, stage, &idx0, &sz0, &pay0, 0, why);
}

static NSData *nvmtl_mesh_translate_fc(NSString *ll, NSString *stage, NSDictionary *fc, NSString **why) {
    size_t n = fc.count;
    if (!n) return nvmtl_mesh_translate(ll, stage, why);
    NSMutableData *st = [NSMutableData dataWithLength:n * (2 * sizeof(uint32_t) + sizeof(void *))];
    uint32_t *idx = st.mutableBytes, *sz = idx + n; const uint8_t **pay = (const uint8_t **)(sz + n);
    NSMutableArray *keep = [NSMutableArray new]; size_t k = 0;
    for (NSString *key in fc) {
        NSArray *b = fc[key];
        if (![b isKindOfClass:[NSArray class]] || !b.count) { if (why) *why = [NSString stringWithFormat:@"function constant %@ has no bytes", key]; return nil; }
        NSMutableData *d = [NSMutableData dataWithLength:b.count];
        for (NSUInteger j = 0; j < b.count; j++) ((uint8_t *)d.mutableBytes)[j] = [b[j] unsignedCharValue];
        [keep addObject:d];
        idx[k] = (uint32_t)key.integerValue; sz[k] = (uint32_t)b.count; pay[k] = d.bytes; k++;
    }
    NSData *r = nvmtl_translate_with_fc(ll, stage, idx, sz, pay, n, why);
    (void)keep;
    return r;
}

static NSString *nvmtl_mesh_text(uint8_t *p, size_t n) {
    if (!p) return nil;
    NSString *s = [[NSString alloc] initWithBytes:p length:(n && p[n-1] == 0) ? n - 1 : n encoding:NSUTF8StringEncoding];
    xlate_free(p, n);
    return s;
}

@implementation NVMTLDevice (B42Mesh)
- (NVMTLRenderPipelineState *)nvmtlMeshVariant:(NVMTLFunction *)mf object:(NVMTLFunction *)of fragment:(id)ffn desc:(MTLMeshRenderPipelineDescriptor *)md
    mode:(uint32_t)mode cap:(uint32_t)cap layout:(uint32_t *)lay kernel:(NVMTLComputePipelineState **)kout object:(NVMTLComputePipelineState **)oout why:(NSString **)why {
    uint8_t *oo = NULL, *ko = NULL, *vo = NULL; size_t on = 0, kn = 0, vn = 0; char e[1024] = {0};
    if (xlate_lower_mesh2(of ? of->_air.UTF8String : NULL, of ? of->_fname.UTF8String : NULL, mf->_air.UTF8String, mf->_fname.UTF8String,
                          mode, cap, &oo, &on, &ko, &kn, &vo, &vn, lay, e, sizeof e) != 0 || !ko || !vo || (mode == 1 && !oo)) {
        if (oo) xlate_free(oo, on); if (ko) xlate_free(ko, kn); if (vo) xlate_free(vo, vn);
        *why = [NSString stringWithFormat:@"\"%@\"%@ did not lower (mode %u): %s", mf->_fname, of ? [NSString stringWithFormat:@" behind \"%@\"", of->_fname] : @"", mode, e[0] ? e : "no reason given"];
        return nil;
    }
    NSString *oll = nvmtl_mesh_text(oo, on), *kll = nvmtl_mesh_text(ko, kn), *vll = nvmtl_mesh_text(vo, vn);
    if (!kll || !vll || (mode == 1 && !oll)) { *why = @"lowered AIR is not UTF-8"; return nil; }
    if (getenv("NVMTL_MESH_DUMP")) {
        NSString *dir = @(getenv("NVMTL_MESH_DUMP"));
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
        [kll writeToFile:[dir stringByAppendingFormat:@"/%@.m%u.kernel.ll", mf->_fname, mode] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        [vll writeToFile:[dir stringByAppendingFormat:@"/%@.m%u.vertex.ll", mf->_fname, mode] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        if (oll) [oll writeToFile:[dir stringByAppendingFormat:@"/%@.object.ll", of->_fname] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    }
    NSString *w = nil; NSError *pe = nil;
    if (mode == 1) {
        NSData *ospv = nvmtl_mesh_translate_fc(oll, @"kernel", of->_fc, &w);
        if (!ospv) { *why = [NSString stringWithFormat:@"object \"%@\" lowered kernel did not translate: %@", of->_fname, w]; return nil; }
        NVMTLFunction *okf = [NVMTLFunction new];
        okf->_spirv = ospv; okf->_fname = of->_fname; okf->_stage = @"kernel"; okf->_air = oll; okf->_lib = of->_lib;
        NVMTLComputePipelineState *op = (NVMTLComputePipelineState *)[self newComputePipelineStateWithFunction:okf error:&pe];
        if (!op) { *why = [NSString stringWithFormat:@"object \"%@\": no compute pipeline (%@)", of->_fname, pe.localizedDescription ?: @"?"]; return nil; }
        *oout = op;
    }
    NSData *kspv = nvmtl_mesh_translate_fc(kll, @"kernel", mf->_fc, &w);
    if (!kspv) { *why = [NSString stringWithFormat:@"\"%@\" lowered kernel (mode %u) did not translate: %@", mf->_fname, mode, w]; return nil; }
    NSData *vspv = nvmtl_mesh_translate(vll, @"vertex", &w);
    if (!vspv) { *why = [NSString stringWithFormat:@"\"%@\" generated vertex (mode %u) did not translate: %@", mf->_fname, mode, w]; return nil; }
    NVMTLFunction *kf = [NVMTLFunction new];
    kf->_spirv = kspv; kf->_fname = mf->_fname; kf->_stage = @"kernel"; kf->_air = kll; kf->_lib = mf->_lib;
    NVMTLComputePipelineState *kp = (NVMTLComputePipelineState *)[self newComputePipelineStateWithFunction:kf error:&pe];
    if (!kp) { *why = [NSString stringWithFormat:@"\"%@\" lowered kernel: no compute pipeline (%@)", mf->_fname, pe.localizedDescription ?: @"?"]; return nil; }
    NVMTLFunction *vf = [NVMTLFunction new];
    vf->_spirv = vspv; vf->_fname = @"m2v_mesh_vs"; vf->_stage = @"vertex"; vf->_air = vll; vf->_lib = mf->_lib;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    if (md.label) d.label = md.label;
    d.vertexFunction = vf; d.fragmentFunction = ffn;
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = md.colorAttachments[i], *b = d.colorAttachments[i];
        b.pixelFormat = a.pixelFormat; b.blendingEnabled = a.blendingEnabled; b.writeMask = a.writeMask;
        b.sourceRGBBlendFactor = a.sourceRGBBlendFactor; b.destinationRGBBlendFactor = a.destinationRGBBlendFactor; b.rgbBlendOperation = a.rgbBlendOperation;
        b.sourceAlphaBlendFactor = a.sourceAlphaBlendFactor; b.destinationAlphaBlendFactor = a.destinationAlphaBlendFactor; b.alphaBlendOperation = a.alphaBlendOperation;
    }
    d.depthAttachmentPixelFormat = md.depthAttachmentPixelFormat; d.stencilAttachmentPixelFormat = md.stencilAttachmentPixelFormat;
    d.rasterSampleCount = md.rasterSampleCount ?: 1; d.alphaToCoverageEnabled = md.alphaToCoverageEnabled;
    d.fragmentLinkedFunctions = md.fragmentLinkedFunctions;
    d.supportIndirectCommandBuffers = md.supportIndirectCommandBuffers;
    NVMTLRenderPipelineState *ps = (NVMTLRenderPipelineState *)[self newRenderPipelineStateWithDescriptor:d error:&pe];
    if (!ps) { *why = [NSString stringWithFormat:@"\"%@\" + \"%@\": no render pipeline (%@)", mf->_fname, [ffn name], pe.localizedDescription ?: @"?"]; return nil; }
    *kout = kp;
    nvlog("mesh pipeline: \"%s\" mode %u -> %s%lu B kernel + %lu B vertex SPIR-V, block %u B (NV %u NP %u k %u) tr %u rs %u cap %u",
          mf->_fname.UTF8String, mode, of ? "object + " : "", (unsigned long)kspv.length, (unsigned long)vspv.length,
          lay[6], lay[0], lay[1], lay[2], lay[7], lay[8], cap);
    return ps;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithMeshDescriptor:(MTLMeshRenderPipelineDescriptor *)md options:(MTLPipelineOption)o
    reflection:(MTLAutoreleasedRenderPipelineReflection *)r error:(NSError **)err {
    if (err) *err = nil;
    if (r) *r = nil;
    id ofn = md.objectFunction, mfn = md.meshFunction, ffn = md.fragmentFunction;
    if (![mfn isKindOfClass:[NVMTLFunction class]] || ![ffn isKindOfClass:[NVMTLFunction class]] || (ofn && ![ofn isKindOfClass:[NVMTLFunction class]]))
        return nvmtl_mesh_fail(err, [NSString stringWithFormat:@"REFUSED - these functions are not ours: object %s, mesh %s, fragment %s",
                                     ofn ? object_getClassName(ofn) : "-", object_getClassName(mfn), object_getClassName(ffn)]);
    NVMTLFunction *mf = (NVMTLFunction *)mfn, *of = (NVMTLFunction *)ofn;
    if (![mf->_stage isEqualToString:@"mesh"] || !mf->_air.length)
        return nvmtl_mesh_fail(err, [NSString stringWithFormat:@"REFUSED - \"%@\" is a %@ function with %lu B of AIR, not a [[mesh]] function",
                                     mf->_fname, mf->_stage, (unsigned long)mf->_air.length]);
    if (of && (![of->_stage isEqualToString:@"object"] || !of->_air.length))
        return nvmtl_mesh_fail(err, [NSString stringWithFormat:@"REFUSED - \"%@\" is a %@ function with %lu B of AIR, not an [[object]] function",
                                     of->_fname, of->_stage, (unsigned long)of->_air.length]);
    { NSString *tw = nvmtl_tg_mismatch(mf, md.maxTotalThreadsPerMeshThreadgroup, "mesh pipeline");
      if (!tw && of) tw = nvmtl_tg_mismatch(of, md.maxTotalThreadsPerObjectThreadgroup, "object pipeline");
      if (tw) return nvmtl_fail(err, tw); }
    if (!nvmtl_xlate_ready() || !xlate_lower_mesh2)
        return nvmtl_mesh_fail(err, @"REFUSED - this translator has no nvmtl_lower_mesh2 (rebuild nvmtl_translate-merge with patch_xl_mesh2.py)");
    /* The object stage (an object kernel filling the scratch, an indirect dispatch of the mesh kernel from it, an indirect
       draw) lost the GPU on the B580: mtl_desc_paths "mesh" (one object thread setting a 1x1x1 grid, a 4-vertex mesh)
       ran 20 s and the device was lost (2026-11-13). Until that runs on the device, the pipeline is refused with an
       error the app sees, not built to hang the GPU; a pipeline without an object stage is unaffected.
       NVMTL_MESH_OBJECT=1 builds it as before. */
    if (of) {
        static int allow = -1;
        if (allow < 0) { const char *v = getenv("NVMTL_MESH_OBJECT"); allow = v && *v && strcmp(v, "0") != 0; }
        if (!allow)
            return nvmtl_mesh_fail(err, [NSString stringWithFormat:@"REFUSED - \"%@\" behind object \"%@\": the object stage is not supported on this "
                                         "device (it lost the GPU); NVMTL_MESH_OBJECT=1 builds it", mf->_fname, of->_fname]);
    }
    uint32_t cap = of ? (uint32_t)(md.maxTotalThreadgroupsPerMeshGrid ?: 1024) : 0;
    NVMTLMeshInfo *mi = [NVMTLMeshInfo new]; mi->_name = mf->_fname; mi->_cap = cap;
    NSString *why = nil; NVMTLComputePipelineState *kp = nil, *op = nil;
    NVMTLRenderPipelineState *ps = [self nvmtlMeshVariant:mf object:of fragment:ffn desc:md mode:(of ? 1 : 0) cap:cap layout:mi->_lay kernel:&kp object:&op why:&why];
    if (!ps) return nvmtl_mesh_fail(err, why);
    mi->_k = kp; mi->_o = op;
    mi->_tgMesh = nvmtl_air_max_tg(mf) ?: md.maxTotalThreadsPerMeshThreadgroup;
    mi->_tgObj = of ? (nvmtl_air_max_tg(of) ?: md.maxTotalThreadsPerObjectThreadgroup) : 0;
    mi->_gridAsk = md.maxTotalThreadgroupsPerMeshGrid;
    ps->_reqMesh = nvmtl_desc_size(md, @selector(requiredThreadsPerMeshThreadgroup));
    ps->_reqObj = of ? nvmtl_desc_size(md, @selector(requiredThreadsPerObjectThreadgroup)) : MTLSizeMake(0, 0, 0);
    if (!of) {
        mi->_capi = 16384;
        NVMTLComputePipelineState *ki = nil, *unused = nil; NSString *w2 = nil;
        NVMTLRenderPipelineState *psi = [self nvmtlMeshVariant:mf object:nil fragment:ffn desc:md mode:2 cap:mi->_capi layout:mi->_layi kernel:&ki object:&unused why:&w2];
        if (psi) { mi->_psi = psi; mi->_ki = ki; }
        else { mi->_indWhy = w2; nvlog("mesh pipeline: \"%s\" has NO indirect variant (%s) - drawMeshThreadgroupsWithIndirectBuffer: will be refused; direct draws are unaffected",
                                       mf->_fname.UTF8String, w2.UTF8String); }
    }
    objc_setAssociatedObject(ps, &kNVMTLMeshInfo, mi, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    nvlog("mesh pipeline: \"%s\"%s%s / fragment %s -> %p (indirect variant %s, %lu+%lu constant(s))", mf->_fname.UTF8String, of ? " behind " : "",
          of ? of->_fname.UTF8String : "", [[ffn name] UTF8String], (__bridge void *)ps, of ? "n/a" : (mi->_psi ? "yes" : "NO"),
          (unsigned long)(of ? of->_fc.count : 0), (unsigned long)mf->_fc.count);
    if (r && ps) {
        NVMTLRenderPipelineReflection *rr = [NVMTLRenderPipelineReflection new];
        rr->_o = of ? nvmtl_lowered_stage_arguments(mi->_o, of) : @[];
        rr->_m = nvmtl_lowered_stage_arguments(mi->_k, mf);
        rr->_f = nvmtl_arguments_for_function((NVMTLFunction *)ffn);
        *r = (MTLRenderPipelineReflection *)rr;
    }
    return ps;
}
- (void)newRenderPipelineStateWithMeshDescriptor:(MTLMeshRenderPipelineDescriptor *)md options:(MTLPipelineOption)o
    completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)h {
    NSError *e = nil; MTLRenderPipelineReflection *rf = nil;
    id<MTLRenderPipelineState> ps = [self newRenderPipelineStateWithMeshDescriptor:md options:o reflection:&rf error:&e];
    if (h) dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ h(ps, rf, e); });
}
@end

@implementation NVMTLRenderCommandEncoder (B42Mesh)
static NSMutableDictionary *nvmtl_mesh_table(NVMTLRenderCommandEncoder *e, BOOL object) {
    NVMTLMeshBinds *mb = objc_getAssociatedObject(e, &kNVMTLMeshBinds);
    if (!mb) { mb = [NVMTLMeshBinds new]; mb->_obj = [NSMutableDictionary new]; mb->_mesh = [NSMutableDictionary new];
               objc_setAssociatedObject(e, &kNVMTLMeshBinds, mb, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
    return object ? mb->_obj : mb->_mesh;
}
static void nvmtl_mesh_note(NVMTLRenderCommandEncoder *e, BOOL object, char kind, NSUInteger i, NSArray *v) {
    NSString *k = [NSString stringWithFormat:@"%c%lu", kind, (unsigned long)i];
    NSMutableDictionary *t = nvmtl_mesh_table(e, object);
    if (v) t[k] = v; else [t removeObjectForKey:k];
}
static void nvmtl_mesh_note_offset(NVMTLRenderCommandEncoder *e, BOOL object, NSUInteger i, NSUInteger off) {
    NSMutableDictionary *t = nvmtl_mesh_table(e, object); NSString *k = [NSString stringWithFormat:@"b%lu", (unsigned long)i];
    NSArray *v = t[k];
    if (v.count == 2) t[k] = @[v[0], @(off)];
    else nvlog("mesh: set%sBufferOffset at %lu with no buffer bound there - ignored", object ? "Object" : "Mesh", (unsigned long)i);
}
static void nvmtl_mesh_replay(NVMTLRenderCommandEncoder *e, NSDictionary *t) {
    for (NSString *k in t) {
        NSArray *v = t[k]; unichar c = [k characterAtIndex:0]; NSUInteger i = (NSUInteger)[[k substringFromIndex:1] integerValue];
        if (c == 'b') {
            if ([v[0] isKindOfClass:[NSData class]]) [e setVertexBytes:[v[0] bytes] length:[v[0] length] atIndex:i];
            else [e setVertexBuffer:v[0] offset:[v[1] unsignedIntegerValue] atIndex:i];
        } else if (c == 't') [e setVertexTexture:v[0] atIndex:i];
        else if (c == 's') {
            if (v.count == 3) [e setVertexSamplerState:v[0] lodMinClamp:[v[1] floatValue] lodMaxClamp:[v[2] floatValue] atIndex:i];
            else [e setVertexSamplerState:v[0] atIndex:i];
        }
    }
}
#define NVMTL_MESH_BINDERS(Stage, OBJ, FWD) \
- (void)set##Stage##Buffer:(id<MTLBuffer>)b offset:(NSUInteger)off atIndex:(NSUInteger)i { nvmtl_mesh_note(self, OBJ, 'b', i, b ? @[b, @(off)] : nil); if (FWD) [self setVertexBuffer:b offset:off atIndex:i]; } \
- (void)set##Stage##BufferOffset:(NSUInteger)off atIndex:(NSUInteger)i { nvmtl_mesh_note_offset(self, OBJ, i, off); if (FWD) [self setVertexBufferOffset:off atIndex:i]; } \
- (void)set##Stage##Buffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger *)o withRange:(NSRange)r { \
    for (NSUInteger k = 0; k < r.length; k++) [self set##Stage##Buffer:b[k] offset:o[k] atIndex:r.location + k]; } \
- (void)set##Stage##Bytes:(const void *)b length:(NSUInteger)l atIndex:(NSUInteger)i { \
    nvmtl_mesh_note(self, OBJ, 'b', i, b ? @[[NSData dataWithBytes:b length:l]] : nil); if (FWD) [self setVertexBytes:b length:l atIndex:i]; } \
- (void)set##Stage##Texture:(id<MTLTexture>)t atIndex:(NSUInteger)i { nvmtl_mesh_note(self, OBJ, 't', i, t ? @[t] : nil); if (FWD) [self setVertexTexture:t atIndex:i]; } \
- (void)set##Stage##Textures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r { \
    for (NSUInteger k = 0; k < r.length; k++) [self set##Stage##Texture:t[k] atIndex:r.location + k]; } \
- (void)set##Stage##SamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { nvmtl_mesh_note(self, OBJ, 's', i, s ? @[s] : nil); if (FWD) [self setVertexSamplerState:s atIndex:i]; } \
- (void)set##Stage##SamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i { \
    nvmtl_mesh_note(self, OBJ, 's', i, s ? @[s, @(lo), @(hi)] : nil); if (FWD) [self setVertexSamplerState:s lodMinClamp:lo lodMaxClamp:hi atIndex:i]; } \
- (void)set##Stage##SamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s withRange:(NSRange)r { \
    for (NSUInteger k = 0; k < r.length; k++) [self set##Stage##SamplerState:s[k] atIndex:r.location + k]; } \
- (void)set##Stage##SamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r { \
    for (NSUInteger k = 0; k < r.length; k++) [self set##Stage##SamplerState:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k] atIndex:r.location + k]; }
NVMTL_MESH_BINDERS(Mesh, NO, YES)
NVMTL_MESH_BINDERS(Object, YES, NO)
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)l atIndex:(NSUInteger)i {
    static int told; if (told++ < 4) nvlog("mesh: setObjectThreadgroupMemoryLength:%lu atIndex:%lu is NOT carried - an object reading that threadgroup memory reads its static size", (unsigned long)l, (unsigned long)i);
}
- (NVMTLBuffer *)nvmtlMeshScratch:(uint64_t)bytes header:(const uint32_t *)h {
    NVMTLBuffer *sb = (NVMTLBuffer *)[(id<MTLDevice>)gNVMTLMainDevice newBufferWithLength:(NSUInteger)bytes options:MTLResourceStorageModePrivate];
    if (!sb) { nvlog("mesh: %llu B scratch buffer allocation FAILED - NOT drawn", bytes); return nil; }
    NVMTLBuffer *hb = [NVMTLBuffer new];
    if (nvmtl_vk_buffer_create(32, 1, &hb->_b) || !hb->_b.map) { nvlog("mesh: header staging buffer FAILED - NOT drawn"); return nil; }
    memcpy(hb->_b.map, h, 32);
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    if (!_cb->_scratch) _cb->_scratch = [NSMutableArray new];
    [_cb->_resources addObject:sb]; [_cb->_scratch addObject:hb];
    nvmtl_vk_cmd_end_render(&_cb->_c);
    nvmtl_vk_cmd_barrier(&_cb->_c);
    if (nvmtl_vk_cmd_fill_buffer(&_cb->_c, &sb->_b, 0, (size_t)bytes, 0)) nvlog("mesh: scratch clear FAILED - counts may be stale");
    nvmtl_vk_cmd_barrier(&_cb->_c);
    if (nvmtl_vk_cmd_copy_buffer(&_cb->_c, &hb->_b, 0, &sb->_b, 0, 32)) nvlog("mesh: header copy FAILED - the draw reads zero args");
    nvmtl_vk_cmd_barrier(&_cb->_c);
    return sb;
}
- (BOOL)nvmtlMeshResume {
    if (![self nvmtlBeginPassLoad:NVMTL_LOAD_ALL]) {
        _begun = NO; nvlog("mesh: render pass could not RESUME after the mesh stage - the rest of this encoder is not drawn"); return NO; }
    nvmtl_vk_cmd_set_cull(&_cb->_c, _cull);
    if (_hasVP) nvmtl_vk_cmd_set_viewport(&_cb->_c, _vp[0], _vp[1], _vp[2], _vp[3], _vp[4], _vp[5]);
    if (_hasSC) nvmtl_vk_cmd_set_scissor(&_cb->_c, (int32_t)_sc[0], (int32_t)_sc[1], _sc[2], _sc[3]);
    [self nvmtlApplyDepthStencil];
    return YES;
}
- (void)nvmtlMeshObjectDraw:(MTLSize)g object:(MTLSize)ot mesh:(MTLSize)mt info:(NVMTLMeshInfo *)mi {
    uint64_t nobj = (uint64_t)g.width * g.height * g.depth;
    if (!nobj || !ot.width || !ot.height || !ot.depth || !mt.width || !mt.height || !mt.depth) return;
    uint32_t tr = mi->_lay[7], rs = mi->_lay[8], block = mi->_lay[6];
    uint64_t bytes = tr + nobj * rs + nobj * (uint64_t)mi->_cap * block;
    if (bytes > 0x7fffffffull) { nvlog("mesh: %llu objects x (%u rec + %u x %u B) = %llu B scratch does not fit - NOT drawn", nobj, rs, mi->_cap, block, bytes); return; }
    uint32_t h[8] = { 0, (uint32_t)nobj, 1, 0,   0, (uint32_t)nobj, 0, 0 };
    NVMTLBuffer *sb = [self nvmtlMeshScratch:bytes header:h];
    if (!sb) return;
    [_cb->_resources addObject:mi->_k]; [_cb->_resources addObject:mi->_o]; [_cb->_resources addObject:_ps];
    NVMTLMeshBinds *mb = objc_getAssociatedObject(self, &kNVMTLMeshBinds);
    nvmtl_mesh_replay(self, mb ? mb->_obj : nil);
    nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, 29, &sb->_b, 0);
    nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, 30, &sb->_b, 0);
    nvmtl_bind_embedded(_cb, NVMTL_SET_VERTEX, mi->_o->_function, _vbufAt, _vbufOff);
    uint32_t ol[3] = { (uint32_t)ot.width, (uint32_t)ot.height, (uint32_t)ot.depth };
    uint32_t tt[3] = { (uint32_t)(g.width * ot.width), (uint32_t)(g.height * ot.height), (uint32_t)(g.depth * ot.depth) };
    nvmtl_vk_cmd_dispatch_threads(&_cb->_c, &mi->_o->_p, tt, ol);
    nvmtl_vk_cmd_barrier(&_cb->_c);
    nvmtl_mesh_replay(self, mb ? mb->_mesh : nil);
    nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, 29, &sb->_b, 0);
    nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, 30, &sb->_b, 0);
    nvmtl_bind_embedded(_cb, NVMTL_SET_VERTEX, mi->_k->_function, _vbufAt, _vbufOff);
    uint32_t ml[3] = { (uint32_t)mt.width, (uint32_t)mt.height, (uint32_t)mt.depth };
    nvmtl_vk_cmd_dispatch_indirect_tg(&_cb->_c, &mi->_k->_p, &sb->_b, 0, ml);
    nvmtl_vk_cmd_barrier(&_cb->_c);
    if (![self nvmtlMeshResume]) return;
    MTLPrimitiveType pt = mi->_lay[2] == 3 ? MTLPrimitiveTypeTriangle : mi->_lay[2] == 2 ? MTLPrimitiveTypeLine : MTLPrimitiveTypePoint;
    static int told; if (told++ < 4) nvlog("mesh: \"%s\" %llu object threadgroup(s) x %ux%ux%u, mesh %ux%ux%u, cap %u -> %llu B scratch", mi->_name.UTF8String,
                                             nobj, ol[0], ol[1], ol[2], ml[0], ml[1], ml[2], mi->_cap, bytes);
    [self drawPrimitives:pt indirectBuffer:(id<MTLBuffer>)sb indirectBufferOffset:16];
}
- (void)drawMeshThreadgroups:(MTLSize)g threadsPerObjectThreadgroup:(MTLSize)ot threadsPerMeshThreadgroup:(MTLSize)mt {
    if (!_begun || !_ps) { nvlog("mesh: draw with no pipeline set"); return; }
    NVMTLMeshInfo *mi = objc_getAssociatedObject(_ps, &kNVMTLMeshInfo);
    if (!mi) { nvlog("mesh: drawMeshThreadgroups with a pipeline that has no mesh stage (%s) - NOT drawn", _ps->_fname.UTF8String); return; }
    if (mi->_o) { [self nvmtlMeshObjectDraw:g object:ot mesh:mt info:mi]; return; }
    uint64_t T = (uint64_t)g.width * g.height * g.depth, bytes = T * mi->_lay[6], verts = T * mi->_lay[1] * mi->_lay[2];
    if (!T || !mt.width || !mt.height || !mt.depth) return;
    if (bytes > 0x7fffffffull || verts > 0xffffffffull) { nvlog("mesh: %llu threadgroups x %u B does not fit - NOT drawn", T, mi->_lay[6]); return; }
    NVMTLBuffer *sb = (NVMTLBuffer *)[(id<MTLDevice>)gNVMTLMainDevice newBufferWithLength:(NSUInteger)bytes options:MTLResourceStorageModePrivate];
    if (!sb) { nvlog("mesh: %llu B scratch buffer allocation FAILED - NOT drawn", bytes); return; }
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    [_cb->_resources addObject:sb]; [_cb->_resources addObject:mi->_k]; [_cb->_resources addObject:_ps];
    nvmtl_vk_cmd_end_render(&_cb->_c);
    nvmtl_vk_cmd_barrier(&_cb->_c);
    if (nvmtl_vk_cmd_fill_buffer(&_cb->_c, &sb->_b, 0, (size_t)bytes, 0)) nvlog("mesh: scratch clear FAILED - counts may be stale");
    nvmtl_vk_cmd_barrier(&_cb->_c);
    nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, 30, &sb->_b, 0);
    nvmtl_bind_embedded(_cb, NVMTL_SET_VERTEX, mi->_k->_function, _vbufAt, _vbufOff);
    uint32_t l[3] = { (uint32_t)mt.width, (uint32_t)mt.height, (uint32_t)mt.depth };
    uint32_t t[3] = { (uint32_t)(g.width * mt.width), (uint32_t)(g.height * mt.height), (uint32_t)(g.depth * mt.depth) };
    nvmtl_vk_cmd_dispatch_threads(&_cb->_c, &mi->_k->_p, t, l);
    nvmtl_vk_cmd_barrier(&_cb->_c);
    if (![self nvmtlBeginPassLoad:NVMTL_LOAD_ALL]) {
        _begun = NO; nvlog("mesh: render pass could not RESUME after the mesh stage - the rest of this encoder is not drawn"); return; }
    nvmtl_vk_cmd_set_cull(&_cb->_c, _cull);
    if (_hasVP) nvmtl_vk_cmd_set_viewport(&_cb->_c, _vp[0], _vp[1], _vp[2], _vp[3], _vp[4], _vp[5]);
    if (_hasSC) nvmtl_vk_cmd_set_scissor(&_cb->_c, (int32_t)_sc[0], (int32_t)_sc[1], _sc[2], _sc[3]);
    [self nvmtlApplyDepthStencil];
    MTLPrimitiveType pt = mi->_lay[2] == 3 ? MTLPrimitiveTypeTriangle : mi->_lay[2] == 2 ? MTLPrimitiveTypeLine : MTLPrimitiveTypePoint;
    static int told; if (told++ < 4) nvlog("mesh: \"%s\" %llu threadgroup(s) x %ux%ux%u -> %llu B scratch, %llu vertices", mi->_name.UTF8String,
                                             T, l[0], l[1], l[2], bytes, verts);
    [self drawPrimitives:pt vertexStart:0 vertexCount:(NSUInteger)verts instanceCount:1 baseInstance:0];
}
- (void)drawMeshThreads:(MTLSize)n threadsPerObjectThreadgroup:(MTLSize)ot threadsPerMeshThreadgroup:(MTLSize)mt {
    if (!mt.width || !mt.height || !mt.depth) return;
    MTLSize g = MTLSizeMake((n.width + mt.width - 1) / mt.width, (n.height + mt.height - 1) / mt.height, (n.depth + mt.depth - 1) / mt.depth);
    [self drawMeshThreadgroups:g threadsPerObjectThreadgroup:ot threadsPerMeshThreadgroup:mt];
}
- (void)drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)b indirectBufferOffset:(NSUInteger)off
    threadsPerObjectThreadgroup:(MTLSize)ot threadsPerMeshThreadgroup:(MTLSize)mt {
    if (!_begun || !_ps) { nvlog("mesh: indirect draw with no pipeline set"); return; }
    NVMTLMeshInfo *mi = objc_getAssociatedObject(_ps, &kNVMTLMeshInfo);
    if (!mi) { nvlog("mesh: drawMeshThreadgroupsWithIndirectBuffer: with a pipeline that has no mesh stage (%s) - NOT drawn", _ps->_fname.UTF8String); return; }
    static int told42c;
    if (mi->_o) { if (told42c++ < 4) nvlog("mesh: \"%s\" object stage + INDIRECT draw is batch 42c - NOT drawn", mi->_name.UTF8String); return; }
    if (!mi->_psi) { if (told42c++ < 4) nvlog("mesh: \"%s\" has no indirect variant (%s) - NOT drawn", mi->_name.UTF8String, mi->_indWhy.UTF8String ?: "?"); return; }
    NVMTLBuffer *ab = (NVMTLBuffer *)b;
    if (!ab || off + 12 > ab->_b.size) { nvlog("mesh: indirect buffer %p offset %lu does not hold 12 bytes - NOT drawn", (__bridge void *)b, (unsigned long)off); return; }
    if (!mt.width || !mt.height || !mt.depth) return;
    uint32_t tr = mi->_layi[7], block = mi->_layi[6];
    uint64_t bytes = tr + 16 + (uint64_t)mi->_capi * block;
    uint32_t h[8] = { 0, 1, 1, 0,   0, 1, 0, 0 };
    nvmtl_buffer_sync_in(ab);
    nvmtl_retain_resource(_cb, ab);
    NVMTLBuffer *sb = [self nvmtlMeshScratch:bytes header:h];
    if (!sb) return;
    if (nvmtl_vk_cmd_copy_buffer(&_cb->_c, &ab->_b, off, &sb->_b, tr, 12)) nvlog("mesh: indirect grid copy FAILED - the kernel reads a zero grid");
    nvmtl_vk_cmd_barrier(&_cb->_c);
    [_cb->_resources addObject:mi->_ki]; [_cb->_resources addObject:mi->_psi];
    nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, 30, &sb->_b, 0);
    nvmtl_bind_embedded(_cb, NVMTL_SET_VERTEX, mi->_ki->_function, _vbufAt, _vbufOff);
    uint32_t ml[3] = { (uint32_t)mt.width, (uint32_t)mt.height, (uint32_t)mt.depth };
    nvmtl_vk_cmd_dispatch_indirect_tg(&_cb->_c, &mi->_ki->_p, &ab->_b, off, ml);
    nvmtl_vk_cmd_barrier(&_cb->_c);
    if (![self nvmtlMeshResume]) return;
    MTLPrimitiveType pt = mi->_layi[2] == 3 ? MTLPrimitiveTypeTriangle : mi->_layi[2] == 2 ? MTLPrimitiveTypeLine : MTLPrimitiveTypePoint;
    static int told; if (told++ < 4) nvlog("mesh: \"%s\" INDIRECT from %p+%lu, mesh %ux%ux%u, cap %u -> %llu B scratch", mi->_name.UTF8String,
                                             (__bridge void *)b, (unsigned long)off, ml[0], ml[1], ml[2], mi->_capi, bytes);
    NVMTLRenderPipelineState *keep = _ps; _ps = mi->_psi;
    [self drawPrimitives:pt indirectBuffer:(id<MTLBuffer>)sb indirectBufferOffset:16];
    _ps = keep;
}
@end
