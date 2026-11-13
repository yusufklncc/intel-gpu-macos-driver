// SPDX-License-Identifier: MIT
// dbuf_model: a host model of the Metal plugin's descriptor-buffer model (nvmtl_db_stage in nullmoth/plugin/nvmtl_dbuf.c)
// that runs the plugin's own decision functions (nullmoth/plugin/nvmtl_dbuf_reuse.h) without Vulkan or a GPU, on any Mac.
// The classic model has the same kind of check in dirty_model.c.
//
// Modelled: binding slots by (kind, pointer, offset) as nvmtl_bind_slot compares them, with the changed bits and dirty
// flag it sets; objects destroyed and made again at the same address (the object generation the plugin bumps); per
// stage layouts (plans) with random slot sets, with or without an address table, read by pipelines that need part or all
// of the table (stage 1d); the table either in the stage region or in a set of its own (the table set, 13 Nov: compute's
// set 1, graphics sets 4 and 5), where a new table is a one-descriptor region apart from the stage region; the per-slot
// shadow descriptors (made only when not valid); regions written into pages, the current region and the MRU of the last
// four, a page change now and then, and a new command buffer. Two stages, each with its own slots, regions and table
// (dbuftess): vertex (or post-tessellation) and fragment, or a kernel's set 0 and set 1; a pipeline may have no stage-1
// region (a kernel whose module names no set 1).
// Checked after every draw: the region the stage points at holds, for every slot of its plan, the descriptor of the
// object that slot names now, and its table (in the region or in the table set) holds the current buffer addresses and
// texture sizes of the entries the pipeline reads.
//
//   clang -O2 -Wall -o dbuf_model dbuf_model.c && ./dbuf_model [steps=2000000] [seed=1] [variant]
//   variant: keep (default: the plugin, every recent region survives a change it does not touch), keepcur
//            (NVMTL_DB_KEEP=cur: only the current one, the rule of 2026-11-12), nokeep (NVMTL_DB_KEEP=0), intable
//            (NVMTL_DB_TABSET=0: every table in its stage region, as before 13 Nov), base (NVMTL_DB_TABSET=base: the
//            tables of tessellation and of kernels that name set 1 in the region, as before dbuftess), and four
//            deliberately broken ones that must FAIL, to show what the model catches: notable (a kept region's table
//            ignored), noslots (its slots ignored), tabstale (a table set's table survives every change), tabwhich (stage
//            1's table set kept in stage 0's bookkeeping), nowide (a buffer's wide change - another window - ignored)
//   ./dbuf_model geek [steps] [seed] [variant] [tablekernels] [set1kernels] [ssbo]
//            a Geekbench-like compute chain instead of random draws: a few kernels in turn, each dispatch preceded by
//            setBuffer with new offsets and a setBytes (a new ring offset), a texture now and then. The kernels read
//            their buffers from pushed entries (cs-table-pc) and their textures from the region; tablekernels (0..4)
//            of the four read the address table instead (in the table set; with intable, in the region). set1kernels
//            (0..4) of them name set 1: a set-1 region of their own, and a table set past the heap holding stage 0's
//            table (even k) or stage 1's (odd k); with base, in the region. Prints stage regions and tables per bind.
//            ssbo (dbufgeek): 0 the kernels' input and output are pushed entries (the assumption until 13 Nov), 1 storage-
//            buffer descriptors at the offset (what the translator leaves; NVMTL_DB_BUFOFF=0), 2 descriptors at the
//            offset's 256-byte window (bufoff, the plugin's default now)
//   ./dbuf_model tess [steps] [seed] [variant] [tesspipelines=2] [tables=1]
//            draws of four render pipelines in turn, the first tesspipelines tessellated (their post-tessellation
//            function reads the whole table: set 4, or with base the region), with per-draw vertex bytes, a vertex
//            buffer offset and fragment bytes. tables 0: no stage reads a table (the baseline).
//   ./dbuf_model fmt     checks the DESCSTAT line format (nvmtl_desc_stats.h) instead
//   clang -O1 -g -fsanitize=address,undefined -o dbuf_model_asan dbuf_model.c && ./dbuf_model_asan 200000
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* nvmtl_vk.c's binding space */
#define NVMTL_NBUF 32
#define NVMTL_NTEX 128
#define NVMTL_NSAMP 32
#define NVMTL_ADDRESS_BINDING (NVMTL_NBUF + NVMTL_NTEX + NVMTL_NSAMP)
#define NVMTL_NSTEX 128
#define NVMTL_STEX_BASE (NVMTL_ADDRESS_BINDING + 1)
#define NVMTL_NBIND (NVMTL_STEX_BASE + NVMTL_NSTEX)
#define NVMTL_CHG_WORDS ((NVMTL_NBIND + 63) / 64)
#include "../../../nullmoth/plugin/nvmtl_dbuf_reuse.h"
#include "../../../nullmoth/plugin/nvmtl_desc_stats.h"

enum { NONE = 0, BUF, TEX, SAMP };
enum { NADDR = 6, NPLAN = 12, NPIPE = 16, NREG = 4096, DUMMY = -1, NST = 2 };

static uint64_t rng = 1;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static uint32_t below(uint32_t n) { return rnd() % n; }

static int64_t live_at[4][NADDR];
static int64_t next_id = 1;
static uint64_t objgen = 1;

/* two stages (`which`), each with its own slots: vertex (or post-tessellation) and fragment, or a kernel's set 0 and set 1 */
typedef struct { uint8_t kind; uint8_t addr; uint32_t off; } slot_t;
static slot_t slots[NST][NVMTL_NBIND];
static uint64_t chg[NST][NVMTL_CHG_WORDS], wchg[NST]; static int dirty[NST];

static int want_kind(uint32_t i) { return i < NVMTL_NBUF ? BUF : i < NVMTL_NBUF + NVMTL_NTEX ? TEX : i < NVMTL_ADDRESS_BINDING ? SAMP : TEX; }
/* what nvmtl_db_gen makes for a slot, as the object it names now (the dummy when nothing of the right kind is there) */
static int64_t desc_of(int w, uint32_t i)
{
    const slot_t *s = &slots[w][i];
    if (s->kind != want_kind(i)) return DUMMY;
    const int64_t id = live_at[s->kind][s->addr];
    return s->kind == BUF ? id * (1ll << 24) + s->off : id;
}
/* bufoff (nvmtl_bufoff_pc.h): what an NVMTL_DBT_SSBOW descriptor holds - the buffer from its offset's 256-byte window
   (the offset rounded down to the 4-byte storage alignment first); the kernel adds the rest from its pushed q */
static int64_t wdesc_of(int w, uint32_t i)
{
    const slot_t *s = &slots[w][i];
    if (s->kind != BUF) return DUMMY;
    return live_at[BUF][s->addr] * (1ll << 24) + (1ll << 23) + ((s->off & ~3u) & 255u);
}
/* nvmtl_tab_entry: entry b < NVMTL_NBUF a buffer's address + offset (the dummy's otherwise), entry NVMTL_NBUF + k texture
   k's size (0 when none) */
static int64_t tab_of(int w, uint32_t i)
{
    if (i < NVMTL_NBUF) return desc_of(w, i);
    return slots[w][i].kind == TEX ? live_at[TEX][slots[w][i].addr] * 7 : 0;
}

/* as nvmtl_bind_slot, nvmtl_vk_bind_buffer_offset and nvmtl_vk_bind_buffer: chg for any change, wchg for a buffer slot's
   wide change (another buffer, or the offset mod 256) */
static void mark(int w, uint32_t i) { dirty[w] = 1; chg[w][i >> 6] |= 1ull << (i & 63); if (i < NVMTL_NBUF) wchg[w] |= 1ull << i; }
static void bind(int w, uint32_t i, uint8_t kind, uint8_t addr, uint32_t off)
{
    slot_t *s = &slots[w][i];
    if (s->kind != kind || s->addr != addr) mark(w, i);
    else if (s->off != off) {
        dirty[w] = 1; chg[w][i >> 6] |= 1ull << (i & 63);
        if (i < NVMTL_NBUF && ((s->off ^ off) & 255u)) wchg[w] |= 1ull << i;
    }
    s->kind = kind; s->addr = addr; s->off = off;
}

/* table: 0 none, 1 in the stage region, 2 in the table set (13 Nov: a vertex, fragment or compute stage whose bindings
   are known), 3 in the table set since dbuftess (a tessellation pipeline's post-tessellation function, set 4; a kernel
   whose module names set 1, a free set from 3) */
typedef struct { uint64_t smask[NVMTL_CHG_WORDS]; uint64_t wmask; int table; } plan_t;   /* wmask: bufoff buffers (not in smask) */
static const int tabset_plan[NST] = { 1, 2 };   /* the table plans' identities, one per stage (nvmtl_db_table_plan) */
static plan_t plans[NPLAN];
/* plan[w] < 0: the pipeline has no region in stage w (a kernel whose module names no set 1: its set 1 is the table set or
   empty); need[w]: the table entries stage w reads (NVMTL_TABNEED form, 0 = the whole table) */
typedef struct { int plan[NST]; uint32_t need[NST]; } pipe_t;
static pipe_t pipes[NPIPE];

/* regions: what was copied into each (offsets are region numbers here) and the page generation it is in */
typedef struct { int64_t d[NVMTL_NBIND]; int64_t tab[NVMTL_NBUF + NVMTL_NTEX]; uint32_t gen; int plan; uint32_t need; } region_t;
static region_t *regions; static uint32_t nreg;
static uint32_t page_gen = 1;
static nvmtl_dbr_stage st[NST];
static int64_t shadow[NST][NVMTL_NBIND]; static uint8_t shwide[NST][NVMTL_NBIND];   /* the shadow and its type (as s->ty) */
/* 0 keep, 1 nokeep, 2 notable, 3 noslots, 4 keepcur, 5 intable, 6 tabstale, 7 base, 8 tabwhich, 9 nowide */
static int variant;

static long failures, draws, found, kept, written, made, aba, pages, tabs, tabfound;
/* a plan's table as this variant runs it: intable (NVMTL_DB_TABSET=0) puts every table set's table back in the region,
   base (NVMTL_DB_TABSET=base) those of tessellation and of kernels that name set 1 */
static int table_of(const plan_t *pl)
{
    if (pl->table >= 2 && variant == 5) return 1;
    if (pl->table == 3 && variant == 7) return 1;
    return pl->table >= 2 ? 2 : pl->table;
}

static void new_cb(void)
{
    memset(st, 0, sizeof st);
    memset(slots, 0, sizeof slots); memset(chg, 0, sizeof chg); memset(wchg, 0, sizeof wchg); memset(dirty, 0, sizeof dirty);   /* a pooled binding table is all zero */
    nreg = 0; page_gen = 1;
}

/* stage w of pipeline p: r its region, tr the table set's region (NULL when the plan's table, if any, is in r) */
static void check(int w, const pipe_t *p, const region_t *r, const region_t *tr, long step)
{
    const plan_t *pl = &plans[p->plan[w]];
    long bad = 0; uint32_t first = 0;
    for (uint32_t i = 0; i < NVMTL_NBIND; i++)
        if (((pl->smask[i >> 6] >> (i & 63)) & 1) && r->d[i] != desc_of(w, i)) { if (!bad++) first = i; }
    for (uint32_t i = 0; i < NVMTL_NBUF; i++)
        if (((pl->wmask >> i) & 1) && r->d[i] != wdesc_of(w, i)) { if (!bad++) first = i; }
    const region_t *t = table_of(pl) == 1 ? r : table_of(pl) == 2 ? tr : NULL;
    if (table_of(pl) == 2 && !tr) { if (!bad++) first = 9998; }
    if (t) {
        const uint32_t need = p->need[w];
        const uint32_t nb = need ? (need & 0xffu) : NVMTL_NBUF, nt = need ? ((need >> 8) & 0xffu) : NVMTL_NTEX;
        for (uint32_t b = 0; b < nb; b++) if (t->tab[b] != tab_of(w, b)) { if (!bad++) first = 1000 + b; }
        for (uint32_t k = 0; k < nt; k++) if (t->tab[NVMTL_NBUF + k] != tab_of(w, NVMTL_NBUF + k)) { if (!bad++) first = 1000 + NVMTL_NBUF + k; }
        if (t->gen != page_gen) { if (!bad++) first = 9999; }
    }
    if (r->gen != page_gen) { if (!bad++) first = 9999; }
    if (!bad) return;
    if (failures < 10) fprintf(stderr, "step %ld stage %d: %ld stale entr%s, first %s %u\n", step, w, bad, bad == 1 ? "y" : "ies",
                               first == 9999 ? "page" : first == 9998 ? "table set (none)" : first >= 1000 ? "table entry" : "slot",
                               first >= 9998 ? 0 : first >= 1000 ? first - 1000 : first);
    failures++;
}

/* nvmtl_db_table_set: stage w's table when it still serves, else a new table in a region of its own. tabwhich (broken):
   stage 1's table is looked up and noted in stage 0's bookkeeping */
static const region_t *table_set(int w, const pipe_t *p)
{
    nvmtl_dbr_stage *ts = &st[variant == 8 ? 0 : w];
    const void *tp = &tabset_plan[variant == 8 ? 0 : w];
    if (nvmtl_dbr_tab_find(ts, tp, page_gen, p->need[w])) { tabfound++; return &regions[ts->tab.off]; }
    if (nreg == NREG) { fprintf(stderr, "out of regions\n"); exit(2); }
    region_t *t = &regions[nreg];
    memset(t->d, 0xee, sizeof t->d); memset(t->tab, 0xee, sizeof t->tab);
    const uint32_t need = p->need[w];
    const uint32_t nb = need ? (need & 0xffu) : NVMTL_NBUF, nt = need ? ((need >> 8) & 0xffu) : NVMTL_NTEX;
    for (uint32_t b = 0; b < nb; b++) t->tab[b] = tab_of(w, b);
    for (uint32_t k = 0; k < nt; k++) t->tab[NVMTL_NBUF + k] = tab_of(w, NVMTL_NBUF + k);
    t->gen = page_gen; t->plan = -1; t->need = need;
    nvmtl_dbr_tab_note(ts, tp, nreg, page_gen, need);
    nreg++; tabs++;
    return t;
}

/* nvmtl_db_stage: the changes stage w saw since, then its region (an earlier one, or a new one) */
static const region_t *stage_region(int w, const pipe_t *p)
{
    const plan_t *pl = &plans[p->plan[w]];
    const int tk = table_of(pl);
    /* a region without a table holds no entries, so it serves any need (the plugin passes need 0) */
    const uint32_t rneed = tk == 1 ? p->need[w] : 0;
    nvmtl_dbr_stage *s = &st[w];
    if (dirty[w]) {
        nvmtl_dbr_region save = s->cur; const nvmtl_dbr_tab tsave = s->tab;
        static const uint64_t none[NVMTL_CHG_WORDS];
        if (variant == 2) s->cur.table = 0;      /* broken: a kept region's table is not looked at */
        if (variant == 3) s->cur.smask = none;   /* broken: its slots are not looked at */
        /* nowide (broken): the wide changes are not passed on, as if every offset change kept the window */
        if (nvmtl_dbr_changed(s, chg[w], variant == 9 ? 0 : wchg[w], page_gen, variant == 1 ? 0 : variant == 4 ? 1 : 2)) kept++;
        if (variant == 2 || variant == 3) { s->cur.table = save.table; s->cur.smask = save.smask; }
        if (variant == 6) s->tab.live = tsave.live;   /* broken: the table set's table survives every change */
        memset(chg[w], 0, sizeof chg[w]); wchg[w] = 0; dirty[w] = 0;
    }
    nvmtl_dbr_objgen(s, objgen);
    if (nvmtl_dbr_find(s, pl, page_gen, rneed)) { found++; return &regions[s->cur.off]; }
    if (nreg == NREG) { fprintf(stderr, "out of regions\n"); exit(2); }
    region_t *r = &regions[nreg];
    memset(r->d, 0xee, sizeof r->d); memset(r->tab, 0xee, sizeof r->tab);
    for (uint32_t i = 0; i < NVMTL_NBIND; i++) {
        const int wide = i < NVMTL_NBUF && ((pl->wmask >> i) & 1);
        if (!wide && !((pl->smask[i >> 6] >> (i & 63)) & 1)) continue;
        if (!nvmtl_dbr_shadow_ok(s, i, wide) || shwide[w][i] != wide) {   /* as nvmtl_db_stage: made again when not valid or of the other type */
            shadow[w][i] = wide ? wdesc_of(w, i) : desc_of(w, i); nvmtl_dbr_shadow_made(s, i, wide); shwide[w][i] = (uint8_t)wide; made++;
        }
        r->d[i] = shadow[w][i];
    }
    if (tk == 1) {   /* nvmtl_address_snapshot with the stage's need: only those entries */
        const uint32_t nb = rneed ? (rneed & 0xffu) : NVMTL_NBUF, nt = rneed ? ((rneed >> 8) & 0xffu) : NVMTL_NTEX;
        for (uint32_t b = 0; b < nb; b++) r->tab[b] = tab_of(w, b);
        for (uint32_t k = 0; k < nt; k++) r->tab[NVMTL_NBUF + k] = tab_of(w, NVMTL_NBUF + k);
    }
    r->gen = page_gen; r->plan = p->plan[w]; r->need = rneed;
    nvmtl_dbr_note(s, pl, pl->smask, pl->wmask, tk == 1, nreg, page_gen, rneed);
    nreg++; written++;
    return r;
}

/* one bind, in the plugin's order: the stage regions (nvmtl_db_stage), then the table sets (nvmtl_db_table_set) */
static void draw(const pipe_t *p, long step)
{
    draws++;
    const region_t *r[NST] = { NULL, NULL }, *t[NST] = { NULL, NULL };
    for (int w = 0; w < NST; w++) if (p->plan[w] >= 0) r[w] = stage_region(w, p);
    for (int w = 0; w < NST; w++) if (p->plan[w] >= 0 && table_of(&plans[p->plan[w]]) == 2) t[w] = table_set(w, p);
    for (int w = 0; w < NST; w++) if (p->plan[w] >= 0) check(w, p, r[w], t[w], step);
}

static void make_plans(void)
{
    for (int k = 0; k < NPLAN; k++) {
        plan_t *p = &plans[k]; memset(p, 0, sizeof *p);
        const int tableOnly = below(2) == 0;   /* buffers only through the table (or pushed): the common GL case */
        for (uint32_t i = 0; i < NVMTL_NBIND; i++) {
            if (i == NVMTL_ADDRESS_BINDING || (i < NVMTL_NBUF && tableOnly)) continue;
            const uint32_t odds = i < NVMTL_NBUF ? 10 : i < NVMTL_ADDRESS_BINDING ? 24 : 60;
            if (below(odds) == 0) p->smask[i >> 6] |= 1ull << (i & 63);
        }
        /* some read no table at all (every entry pushed: table-pc); of those that do, a few keep it in the region (the
           plugin does when a stage's bindings are unknown or its module is not as expected), the others have a table
           set: the 13 Nov cases, or since dbuftess tessellation and kernels that name set 1 */
        p->table = below(4) == 0 ? 0 : below(6) == 0 ? 1 : below(2) ? 2 : 3;
        /* bufoff: one plan in three holds its buffer descriptors at their offset's window (a converted kernel's) */
        if (below(3) == 0) { p->wmask = p->smask[0] & ((1ull << NVMTL_NBUF) - 1); p->smask[0] &= ~p->wmask; }
    }
    for (int k = 0; k < NPIPE; k++) {
        /* a render pipeline (both stages), a kernel (stage 0 only) or a kernel that names set 1 (both) */
        pipes[k].plan[0] = (int)below(NPLAN);
        pipes[k].plan[1] = below(4) == 0 ? -1 : (int)below(NPLAN);
        for (int w = 0; w < NST; w++) {
            const uint32_t r = below(4);
            pipes[k].need[w] = r == 0 ? 0 : (0x80000000u | (below(NVMTL_NBUF) + 1) | ((below(16)) << 8));   /* NVMTL_TABNEED form */
        }
    }
}

static int fmt_check(void)
{
    nvmtl_dstat d; memset(&d, 0, sizeof d);
    d.v[NVMTL_DS_DRAW] = 1000; d.v[NVMTL_DS_DISP] = 0; d.v[NVMTL_DS_TICKS] = 250000; d.v[NVMTL_DS_REGION] = 500;
    d.v[NVMTL_DS_BYTES] = 128000; d.v[NVMTL_DS_LEGACY] = 0; d.v[NVMTL_DS_TABLES] = 250;
    char b[1400];
    const int n = nvmtl_dstat_format(b, sizeof b, "buffer", "5.0 s", &d, 1.0);
    printf("%s\n", b);
    const char *want[] = { "buffer 5.0 s: 1000 draws + 0 dispatches bound, 250 ns each (0.2 ms)", "regions 500 new", "125.0 KB",
                           "0 legacy calls", "per bind: 0.00 sets, 0.50 regions, 0.25 tables, 128 B" };
    int bad = n <= 0 || n >= (int)sizeof b;
    for (unsigned k = 0; k < sizeof want / sizeof want[0]; k++) if (!strstr(b, want[k])) { fprintf(stderr, "format: missing \"%s\"\n", want[k]); bad = 1; }
    printf("dbuf_model fmt: %s (%d chars)\n", bad ? "FAILED" : "ok", n);
    return bad;
}

static int parse_variant(const char *v)
{
    return !strcmp(v, "nokeep") ? 1 : !strcmp(v, "notable") ? 2 : !strcmp(v, "noslots") ? 3 : !strcmp(v, "keepcur") ? 4 :
           !strcmp(v, "intable") ? 5 : !strcmp(v, "tabstale") ? 6 : !strcmp(v, "base") ? 7 : !strcmp(v, "tabwhich") ? 8 :
           !strcmp(v, "nowide") ? 9 : 0;
}
static const char *vn[] = { "", " (NVMTL_DB_KEEP=0)", " (BROKEN: kept table ignored)", " (BROKEN: kept slots ignored)", " (NVMTL_DB_KEEP=cur)",
                            " (NVMTL_DB_TABSET=0)", " (BROKEN: table set never renewed)", " (NVMTL_DB_TABSET=base)",
                            " (BROKEN: stage 1's table set kept as stage 0's)", " (BROKEN: wide buffer changes ignored)" };
static void set_bit(plan_t *p, uint32_t i) { p->smask[i >> 6] |= 1ull << (i & 63); }

/* Geekbench 6 Metal, as its DESCSTAT lines show it: dispatches only, each with new buffer offsets and setBytes, a few
   kernels in turn (a chain: A writes, B reads it, ...). Kernel k reads textures 0..k from its region; its buffers 0..3
   come from pushed entries (no table: cs-table-pc), or, for the first `tk` kernels, from the address table (in the
   table set: compute's set 1; with intable, in the region).
   dbuftess: the first `s1` kernels name set 1. Their set-1 region holds a stage-1 texture and buffer (slots that change
   1 dispatch in 64); a table-reading one reads stage 0's table (even k) or stage 1's (odd k), in a free set from 3
   (with base or intable, in the region).
   dbufgeek: ssbo says how the kernels hold their input and output (buffers 0 and 1). 0: as pushed entries, nothing in the
   region (the shape assumed until the 13 Nov night); 1: as storage-buffer descriptors at the bound offset in the region,
   what the translator really leaves (it keeps a plainly indexed buffer as a descriptor) and the plugin did until
   dbufgeek (NVMTL_DB_BUFOFF=0); 2: as descriptors at their offset's 256-byte window with the rest pushed (bufoff, the
   default now). The offsets are mostly multiples of 256 (Geekbench's, and mtl_desc_cost geek's 1024); one in 32 is
   not, so the window changes too. */
static int geek(long steps, int tk, int s1, int ssbo)
{
    enum { NK = 4 };
    memset(plans, 0, sizeof plans);
    for (int k = 0; k < NK; k++) {
        const int set1 = k < s1, reads = k < tk, t1 = set1 && (k & 1);
        plan_t *p = &plans[k];
        for (int j = 0; j <= k; j++) set_bit(p, NVMTL_NBUF + j);
        if (ssbo == 1) { set_bit(p, 0); set_bit(p, 1); }
        if (ssbo == 2) p->wmask = 3;
        p->table = reads && !t1 ? (set1 ? 3 : 2) : 0;
        pipes[k].plan[0] = k; pipes[k].need[0] = reads && !t1 ? (0x80000000u | 4u) : (0x80000000u | 0u);
        pipes[k].plan[1] = -1; pipes[k].need[1] = 0;
        if (set1) {
            plan_t *q = &plans[NK + k];
            memset(q, 0, sizeof *q); set_bit(q, NVMTL_NBUF); set_bit(q, 3);
            q->table = t1 && reads ? 3 : 0;
            pipes[k].plan[1] = NK + k;   /* need[1] 0: compute's stage 1 reads the whole table (tabneed[1] is never made) */
        }
    }
    #define GEEK_CB() do { new_cb(); for (int j = 0; j < NK; j++) bind(0, NVMTL_NBUF + j, TEX, (uint8_t)j, 0); \
                           if (s1) { bind(1, NVMTL_NBUF, TEX, 0, 0); bind(1, 3, BUF, 3, 0); } } while (0)
    GEEK_CB();
    long disp = 0, w0 = written, t0 = tabs;
    for (long step = 0; step < steps; step++) {
        const int k = (int)(step % NK);
        bind(0, 0, BUF, 0, (uint32_t)below(64) * 256 + (below(32) == 0 ? 16 * below(16) : 0));   /* setBuffer:offset: (input) */
        bind(0, 1, BUF, 1, (uint32_t)below(64) * 256 + (below(32) == 0 ? 16 * below(16) : 0));   /* setBuffer:offset: (output) */
        bind(0, 2, BUF, 2, (uint32_t)(step & 255) * 64);               /* setBytes: the bytes ring's next offset */
        if (below(16) == 0) bind(0, NVMTL_NBUF + below(NK), TEX, (uint8_t)below(NADDR), 0);   /* setTexture now and then */
        if (s1) {   /* the set-1 slots: one the region holds, one only the table does */
            if (below(64) == 0) bind(1, 3, BUF, 3, (uint32_t)below(64) * 256);
            if (below(64) == 0) bind(1, 0, BUF, 0, (uint32_t)below(64) * 256);
        }
        if (below(2000) == 0) { page_gen++; pages++; }
        if (below(5000) == 0 || nreg > NREG - 8) GEEK_CB();
        draw(&pipes[k], step); disp++;
    }
    #undef GEEK_CB
    static const char *const sn[3] = { "pushed", "descriptors at the offset", "descriptors at the window (bufoff)" };
    printf("dbuf_model geek%s, %d of %d kernels reading the table, %d naming set 1, buffers %s: %ld dispatches, %ld new regions = %.3f regions per bind, "
           "%ld found an earlier region, %ld changes kept the current one; %ld table-set tables = %.3f per bind: %ld FAILED\n",
           vn[variant], tk, NK, s1, sn[ssbo], disp, written - w0, (double)(written - w0) / (double)(disp ? disp : 1), found, kept,
           tabs - t0, (double)(tabs - t0) / (double)(disp ? disp : 1), failures);
    return failures ? 1 : 0;
}

/* dbuftess: draws in a scene with tessellation. Four render pipelines in turn; the first `tp` are tessellation pipelines
   whose post-tessellation function reads its buffers (setVertexBytes uniforms, the control points) through the address
   table, the whole of it (table-pc is off for tessellation, and its table-entry needs are not computed): table type 3,
   the vertex stage's table set (set 4) since dbuftess, the stage region before. Of the others, one in two reads a
   vertex table (table type 2: the 13 Nov table set), the rest push their entries; pipeline 1's fragment stage reads a
   table too. Every draw: setVertexBytes (a new ring offset), setVertexBufferOffset, and one in two setFragmentBytes;
   a fragment texture 1 draw in 16, the post-tessellation function's texture 1 in 64. tables 0: no stage reads a table
   (the baseline). */
static int tess(long steps, int tp, int tables)
{
    enum { NK = 4 };
    memset(plans, 0, sizeof plans);
    for (int k = 0; k < NK; k++) {
        const int ts = k < tp;
        plan_t *v = &plans[k], *f = &plans[NK + k];
        if (ts && k == 0) set_bit(v, NVMTL_NBUF + 8);   /* a displacement map */
        if (!ts) set_bit(v, 4);                          /* a vertex buffer read as a buffer descriptor */
        v->table = !tables ? 0 : ts ? 3 : (k & 1) ? 2 : 0;
        set_bit(f, NVMTL_NBUF + (k & 1)); set_bit(f, NVMTL_NBUF + NVMTL_NTEX);
        f->table = tables && k == 1 ? 2 : 0;
        pipes[k].plan[0] = k; pipes[k].plan[1] = NK + k;
        pipes[k].need[0] = ts ? 0 : (0x80000000u | 2u);
        pipes[k].need[1] = 0x80000000u | 1u;
    }
    #define TESS_CB() do { new_cb(); bind(0, NVMTL_NBUF + 8, TEX, 0, 0); bind(0, 4, BUF, 4, 0); \
                           bind(1, NVMTL_NBUF, TEX, 1, 0); bind(1, NVMTL_NBUF + 1, TEX, 2, 0); bind(1, NVMTL_NBUF + NVMTL_NTEX, SAMP, 0, 0); } while (0)
    TESS_CB();
    long nd = 0, w0 = written, t0 = tabs;
    for (long step = 0; step < steps; step++) {
        const int k = (int)(step % NK);
        bind(0, 0, BUF, 0, (uint32_t)(step & 255) * 64);              /* setVertexBytes: the bytes ring */
        bind(0, 1, BUF, 1, (uint32_t)below(4) * 256);                 /* setVertexBufferOffset: control points or vertices */
        if (step & 1) bind(1, 0, BUF, 2, (uint32_t)(step & 255) * 64); /* setFragmentBytes */
        if (below(16) == 0) bind(1, NVMTL_NBUF + below(2), TEX, (uint8_t)below(NADDR), 0);
        if (below(64) == 0) bind(0, NVMTL_NBUF + 8, TEX, (uint8_t)below(NADDR), 0);
        if (below(2000) == 0) { page_gen++; pages++; }
        if (below(5000) == 0 || nreg > NREG - 8) TESS_CB();
        draw(&pipes[k], step); nd++;
    }
    #undef TESS_CB
    printf("dbuf_model tess%s, %d of %d pipelines tessellated%s: %ld draws, %ld new regions = %.3f regions per bind, "
           "%ld found an earlier region, %ld changes kept the current one; %ld table-set tables = %.3f per bind: %ld FAILED\n",
           vn[variant], tp, NK, tables ? "" : ", no stage reads a table", nd, written - w0, (double)(written - w0) / (double)(nd ? nd : 1),
           found, kept, tabs - t0, (double)(tabs - t0) / (double)(nd ? nd : 1), failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "fmt")) return fmt_check();
    if (argc > 1 && (!strcmp(argv[1], "geek") || !strcmp(argv[1], "tess"))) {
        const long steps = argc > 2 ? atol(argv[2]) : 1000000;
        rng = argc > 3 ? strtoull(argv[3], NULL, 10) * 2654435761ull + 1 : 1;
        if (argc > 4) variant = parse_variant(argv[4]);
        regions = (region_t *)calloc(NREG, sizeof *regions);
        if (!regions) return 2;
        for (int k = 0; k < 4; k++) for (int a = 0; a < NADDR; a++) live_at[k][a] = next_id++;
        const int sb = argc > 7 ? atoi(argv[7]) : 0;
        const int r = argv[1][0] == 'g' ? geek(steps, argc > 5 ? atoi(argv[5]) : 0, argc > 6 ? atoi(argv[6]) : 0, sb >= 0 && sb <= 2 ? sb : 0)
                                        : tess(steps, argc > 5 ? atoi(argv[5]) : 2, argc > 6 ? atoi(argv[6]) : 1);
        free(regions);
        return r;
    }
    const long steps = argc > 1 ? atol(argv[1]) : 2000000;
    rng = argc > 2 ? strtoull(argv[2], NULL, 10) * 2654435761ull + 1 : 1;
    if (argc > 3) variant = parse_variant(argv[3]);
    regions = (region_t *)calloc(NREG, sizeof *regions);
    if (!regions) return 2;
    for (int k = 0; k < 4; k++) for (int a = 0; a < NADDR; a++) live_at[k][a] = next_id++;
    make_plans(); new_cb();
    for (long step = 0; step < steps; step++) {
        const uint32_t op = below(100);
        const int w = (int)below(NST);
        if (op < 10) bind(w, below(NVMTL_NBUF), BUF, (uint8_t)below(NADDR), below(4) == 0 ? below(16) * 68 : below(8) * 256);   /* setBytes / setBuffer */
        else if (op < 20) bind(w, NVMTL_NBUF + below(24), below(10) ? TEX : NONE, (uint8_t)below(NADDR), 0);
        else if (op < 24) bind(w, NVMTL_NBUF + NVMTL_NTEX + below(8), below(10) ? SAMP : NONE, (uint8_t)below(NADDR), 0);
        else if (op < 27) { const uint32_t j = below(4) == 0 ? 60 + below(68) : below(8); bind(w, NVMTL_STEX_BASE + j, below(10) ? TEX : NONE, (uint8_t)below(NADDR), 0); }
        else if (op < 29) {
            /* a texture or sampler destroyed and another made at its address, bound again where the first was */
            const int kind = below(4) ? TEX : SAMP; const uint8_t a = (uint8_t)below(NADDR);
            objgen++; live_at[kind][a] = next_id++; aba++;
            for (int s = 0; s < NST; s++)
                for (uint32_t i = NVMTL_NBUF; i < NVMTL_NBIND; i++) if (slots[s][i].kind == kind && slots[s][i].addr == a) bind(s, i, (uint8_t)kind, a, 0);
        } else if (op < 30) {
            /* a buffer made again at its address: nvmtl_vk_bind_buffer sees the device address change */
            const uint8_t a = (uint8_t)below(NADDR); live_at[BUF][a] = next_id++;
            for (int s = 0; s < NST; s++) for (uint32_t i = 0; i < NVMTL_NBUF; i++) if (slots[s][i].kind == BUF && slots[s][i].addr == a) mark(s, i);
        } else if (op < 31) { page_gen++; pages++; }   /* the page filled up: regions are never reused across pages */
        else if (op < 32 || nreg > NREG - 8) new_cb();
        else draw(&pipes[below(NPIPE)], step);
    }
    printf("dbuf_model%s: %ld binds (%ld stage regions found earlier ones, %ld new regions, %ld slot changes kept the region, "
           "%ld shadow descriptors made; %ld table-set tables, %ld reused), %ld page changes, %ld destroy+remake at the same "
           "address: %ld FAILED\n", vn[variant], draws, found, written, kept, made, tabs, tabfound, pages, aba, failures);
    free(regions);
    return failures ? 1 : 0;
}
