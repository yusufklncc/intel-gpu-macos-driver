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
// four, a page change now and then, and a new command buffer.
// Checked after every draw: the region the stage points at holds, for every slot of its plan, the descriptor of the
// object that slot names now, and its table (in the region or in the table set) holds the current buffer addresses and
// texture sizes of the entries the pipeline reads.
//
//   clang -O2 -Wall -o dbuf_model dbuf_model.c && ./dbuf_model [steps=2000000] [seed=1] [variant]
//   variant: keep (default: the plugin, every recent region survives a change it does not touch), keepcur
//            (NVMTL_DB_KEEP=cur: only the current one, the rule of 2026-11-12), nokeep (NVMTL_DB_KEEP=0), intable
//            (NVMTL_DB_TABSET=0: every table in its stage region, as before 13 Nov), and three deliberately broken ones
//            that must FAIL, to show what the model catches: notable (a kept region's table ignored), noslots (its slots
//            ignored), tabstale (a table set's table survives every change)
//   ./dbuf_model geek [steps] [seed] [variant] [tablekernels]
//            a Geekbench-like compute chain instead of random draws: a few kernels in turn, each dispatch preceded by
//            setBuffer with new offsets and a setBytes (a new ring offset), a texture now and then. The kernels read
//            their buffers from pushed entries (cs-table-pc) and their textures from the region; tablekernels (0..4)
//            of the four read the address table instead (in the table set; with intable, in the region). Prints stage
//            regions and tables per bind.
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
enum { NADDR = 6, NPLAN = 10, NPIPE = 16, NREG = 4096, DUMMY = -1 };

static uint64_t rng = 1;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static uint32_t below(uint32_t n) { return rnd() % n; }

static int64_t live_at[4][NADDR];
static int64_t next_id = 1;
static uint64_t objgen = 1;

typedef struct { uint8_t kind; uint8_t addr; uint32_t off; } slot_t;
static slot_t slots[NVMTL_NBIND];
static uint64_t chg[NVMTL_CHG_WORDS]; static int dirty;

static int want_kind(uint32_t i) { return i < NVMTL_NBUF ? BUF : i < NVMTL_NBUF + NVMTL_NTEX ? TEX : i < NVMTL_ADDRESS_BINDING ? SAMP : TEX; }
/* what nvmtl_db_gen makes for a slot, as the object it names now (the dummy when nothing of the right kind is there) */
static int64_t desc_of(uint32_t i)
{
    const slot_t *s = &slots[i];
    if (s->kind != want_kind(i)) return DUMMY;
    const int64_t id = live_at[s->kind][s->addr];
    return s->kind == BUF ? id * 4096 + s->off : id;
}
/* nvmtl_tab_entry: entry b < NVMTL_NBUF a buffer's address + offset (the dummy's otherwise), entry NVMTL_NBUF + k texture
   k's size (0 when none) */
static int64_t tab_of(uint32_t i)
{
    if (i < NVMTL_NBUF) return desc_of(i);
    return slots[i].kind == TEX ? live_at[TEX][slots[i].addr] * 7 : 0;
}

static void mark(uint32_t i) { dirty = 1; chg[i >> 6] |= 1ull << (i & 63); }
static void bind(uint32_t i, uint8_t kind, uint8_t addr, uint32_t off)
{
    slot_t *s = &slots[i];
    if (s->kind != kind || s->addr != addr || s->off != off) mark(i);
    s->kind = kind; s->addr = addr; s->off = off;
}

/* table: 0 none, 1 in the stage region, 2 in the table set (tabset: the table plan's identity, one per model) */
typedef struct { uint64_t smask[NVMTL_CHG_WORDS]; int table; } plan_t;
static const int tabset_plan = 1;
static plan_t plans[NPLAN];
typedef struct { int plan; uint32_t need; } pipe_t;
static pipe_t pipes[NPIPE];

/* regions: what was copied into each (offsets are region numbers here) and the page generation it is in */
typedef struct { int64_t d[NVMTL_NBIND]; int64_t tab[NVMTL_NBUF + NVMTL_NTEX]; uint32_t gen; int plan; uint32_t need; } region_t;
static region_t *regions; static uint32_t nreg;
static uint32_t page_gen = 1;
static nvmtl_dbr_stage st;
static int64_t shadow[NVMTL_NBIND];
static int variant;   /* 0 keep, 1 nokeep, 2 notable, 3 noslots, 4 keepcur, 5 intable, 6 tabstale */

static long failures, draws, found, kept, written, made, aba, pages, tabs, tabfound;
/* a plan's table as this variant runs it: intable puts every table set's table back in the region */
static int table_of(const plan_t *pl) { return pl->table == 2 && variant == 5 ? 1 : pl->table; }

static void new_cb(void)
{
    memset(&st, 0, sizeof st);
    memset(slots, 0, sizeof slots); memset(chg, 0, sizeof chg); dirty = 0;   /* a pooled binding table is all zero */
    nreg = 0; page_gen = 1;
}

/* tr: the table set's region (NULL when the plan's table, if any, is in r) */
static void check(const pipe_t *p, const region_t *r, const region_t *tr, long step)
{
    const plan_t *pl = &plans[p->plan];
    long bad = 0; uint32_t first = 0;
    for (uint32_t i = 0; i < NVMTL_NBIND; i++)
        if (((pl->smask[i >> 6] >> (i & 63)) & 1) && r->d[i] != desc_of(i)) { if (!bad++) first = i; }
    const region_t *t = table_of(pl) == 1 ? r : table_of(pl) == 2 ? tr : NULL;
    if (table_of(pl) == 2 && !tr) { if (!bad++) first = 9998; }
    if (t) {
        const uint32_t nb = p->need ? (p->need & 0xffu) : NVMTL_NBUF, nt = p->need ? ((p->need >> 8) & 0xffu) : NVMTL_NTEX;
        for (uint32_t b = 0; b < nb; b++) if (t->tab[b] != tab_of(b)) { if (!bad++) first = 1000 + b; }
        for (uint32_t k = 0; k < nt; k++) if (t->tab[NVMTL_NBUF + k] != tab_of(NVMTL_NBUF + k)) { if (!bad++) first = 1000 + NVMTL_NBUF + k; }
        if (t->gen != page_gen) { if (!bad++) first = 9999; }
    }
    if (r->gen != page_gen) { if (!bad++) first = 9999; }
    if (!bad) return;
    if (failures < 10) fprintf(stderr, "step %ld: %ld stale entr%s, first %s %u\n", step, bad, bad == 1 ? "y" : "ies",
                               first == 9999 ? "page" : first == 9998 ? "table set (none)" : first >= 1000 ? "table entry" : "slot",
                               first >= 9998 ? 0 : first >= 1000 ? first - 1000 : first);
    failures++;
}

/* nvmtl_db_table_set: the stage's table when it still serves, else a new table in a region of its own */
static const region_t *table_set(const pipe_t *p)
{
    if (nvmtl_dbr_tab_find(&st, &tabset_plan, page_gen, p->need)) { tabfound++; return &regions[st.tab.off]; }
    if (nreg == NREG) { fprintf(stderr, "out of regions\n"); exit(2); }
    region_t *t = &regions[nreg];
    memset(t->d, 0xee, sizeof t->d); memset(t->tab, 0xee, sizeof t->tab);
    const uint32_t nb = p->need ? (p->need & 0xffu) : NVMTL_NBUF, nt = p->need ? ((p->need >> 8) & 0xffu) : NVMTL_NTEX;
    for (uint32_t b = 0; b < nb; b++) t->tab[b] = tab_of(b);
    for (uint32_t k = 0; k < nt; k++) t->tab[NVMTL_NBUF + k] = tab_of(NVMTL_NBUF + k);
    t->gen = page_gen; t->plan = -1; t->need = p->need;
    nvmtl_dbr_tab_note(&st, &tabset_plan, nreg, page_gen, p->need);
    nreg++; tabs++;
    return t;
}

static void draw(const pipe_t *p, long step)
{
    draws++;
    const plan_t *pl = &plans[p->plan];
    const int tk = table_of(pl);
    /* nvmtl_db_stage: a region without a table holds no entries, so it serves any need (the plugin passes need 0) */
    const uint32_t rneed = tk == 1 ? p->need : 0;
    if (dirty) {   /* nvmtl_db_stage */
        nvmtl_dbr_region save = st.cur; const nvmtl_dbr_tab tsave = st.tab;
        static const uint64_t none[NVMTL_CHG_WORDS];
        if (variant == 2) st.cur.table = 0;      /* broken: a kept region's table is not looked at */
        if (variant == 3) st.cur.smask = none;   /* broken: its slots are not looked at */
        if (nvmtl_dbr_changed(&st, chg, page_gen, variant == 1 ? 0 : variant == 4 ? 1 : 2)) kept++;
        if (variant == 2 || variant == 3) { st.cur.table = save.table; st.cur.smask = save.smask; }
        if (variant == 6) st.tab.live = tsave.live;   /* broken: the table set's table survives every change */
        memset(chg, 0, sizeof chg); dirty = 0;
    }
    nvmtl_dbr_objgen(&st, objgen);
    if (nvmtl_dbr_find(&st, pl, page_gen, rneed)) { found++; check(p, &regions[st.cur.off], tk == 2 ? table_set(p) : NULL, step); return; }
    if (nreg == NREG) { fprintf(stderr, "out of regions\n"); exit(2); }
    region_t *r = &regions[nreg];
    memset(r->d, 0xee, sizeof r->d); memset(r->tab, 0xee, sizeof r->tab);
    for (uint32_t i = 0; i < NVMTL_NBIND; i++) {
        if (!((pl->smask[i >> 6] >> (i & 63)) & 1)) continue;
        if (!nvmtl_dbr_valid(&st, i)) { shadow[i] = desc_of(i); nvmtl_dbr_set_valid(&st, i); made++; }
        r->d[i] = shadow[i];
    }
    if (tk == 1) {   /* nvmtl_address_snapshot with the stage's need: only those entries */
        const uint32_t nb = p->need ? (p->need & 0xffu) : NVMTL_NBUF, nt = p->need ? ((p->need >> 8) & 0xffu) : NVMTL_NTEX;
        for (uint32_t b = 0; b < nb; b++) r->tab[b] = tab_of(b);
        for (uint32_t k = 0; k < nt; k++) r->tab[NVMTL_NBUF + k] = tab_of(NVMTL_NBUF + k);
    }
    r->gen = page_gen; r->plan = p->plan; r->need = rneed;
    nvmtl_dbr_note(&st, pl, pl->smask, tk == 1, nreg, page_gen, rneed);
    nreg++; written++;
    check(p, r, tk == 2 ? table_set(p) : NULL, step);
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
        /* some read no table at all (every entry pushed: table-pc); of those that do, most have the table set (the
           plugin keeps the table in the region when a stage's bindings are unknown: tessellation, a kernel naming set 1) */
        p->table = below(4) == 0 ? 0 : below(4) ? 2 : 1;
    }
    for (int k = 0; k < NPIPE; k++) {
        pipes[k].plan = (int)below(NPLAN);
        const uint32_t r = below(4);
        pipes[k].need = r == 0 ? 0 : (0x80000000u | (below(NVMTL_NBUF) + 1) | ((below(16)) << 8));   /* NVMTL_TABNEED form */
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
           !strcmp(v, "intable") ? 5 : !strcmp(v, "tabstale") ? 6 : 0;
}
static const char *vn[] = { "", " (NVMTL_DB_KEEP=0)", " (BROKEN: kept table ignored)", " (BROKEN: kept slots ignored)", " (NVMTL_DB_KEEP=cur)",
                            " (NVMTL_DB_TABSET=0)", " (BROKEN: table set never renewed)" };

/* Geekbench 6 Metal, as its DESCSTAT lines show it: dispatches only, each with new buffer offsets and setBytes, a few
   kernels in turn (a chain: A writes, B reads it, ...). Kernel k reads textures 0..k from its region; its buffers 0..3
   come from pushed entries (no table: cs-table-pc), or, for the first `tk` kernels, from the address table (in the
   table set: compute's set 1; with intable, in the region). */
static int geek(long steps, int tk)
{
    enum { NK = 4 };
    memset(plans, 0, sizeof plans);
    for (int k = 0; k < NK; k++) {
        plan_t *p = &plans[k];
        for (int j = 0; j <= k; j++) p->smask[(NVMTL_NBUF + j) >> 6] |= 1ull << ((NVMTL_NBUF + j) & 63);
        p->table = k < tk ? 2 : 0;
        pipes[k].plan = k; pipes[k].need = k < tk ? (0x80000000u | 4u) : (0x80000000u | 0u);
    }
    new_cb();
    for (int j = 0; j < NK; j++) bind(NVMTL_NBUF + j, TEX, (uint8_t)j, 0);
    long disp = 0, w0 = written, t0 = tabs;
    for (long step = 0; step < steps; step++) {
        const int k = (int)(step % NK);
        bind(0, BUF, 0, (uint32_t)below(64) * 256);                 /* setBuffer:offset: (input) */
        bind(1, BUF, 1, (uint32_t)below(64) * 256);                 /* setBuffer:offset: (output) */
        bind(2, BUF, 2, (uint32_t)(step & 255) * 64);               /* setBytes: the bytes ring's next offset */
        if (below(16) == 0) bind(NVMTL_NBUF + below(NK), TEX, (uint8_t)below(NADDR), 0);   /* setTexture now and then */
        if (below(2000) == 0) { page_gen++; pages++; }
        if (below(5000) == 0 || nreg > NREG - 8) { new_cb(); for (int j = 0; j < NK; j++) bind(NVMTL_NBUF + j, TEX, (uint8_t)j, 0); }
        draw(&pipes[k], step); disp++;
    }
    printf("dbuf_model geek%s, %d of %d kernels reading the table: %ld dispatches, %ld new regions = %.3f regions per bind, "
           "%ld found an earlier region, %ld changes kept the current one; %ld table-set tables = %.3f per bind: %ld FAILED\n",
           vn[variant], tk, NK, disp, written - w0, (double)(written - w0) / (double)(disp ? disp : 1), found, kept,
           tabs - t0, (double)(tabs - t0) / (double)(disp ? disp : 1), failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "fmt")) return fmt_check();
    if (argc > 1 && !strcmp(argv[1], "geek")) {
        const long steps = argc > 2 ? atol(argv[2]) : 1000000;
        rng = argc > 3 ? strtoull(argv[3], NULL, 10) * 2654435761ull + 1 : 1;
        if (argc > 4) variant = parse_variant(argv[4]);
        regions = (region_t *)calloc(NREG, sizeof *regions);
        if (!regions) return 2;
        for (int k = 0; k < 4; k++) for (int a = 0; a < NADDR; a++) live_at[k][a] = next_id++;
        const int r = geek(steps, argc > 5 ? atoi(argv[5]) : 0);
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
        if (op < 10) bind(below(NVMTL_NBUF), BUF, (uint8_t)below(NADDR), below(4) * 256);   /* setBytes / setBuffer */
        else if (op < 20) bind(NVMTL_NBUF + below(24), below(10) ? TEX : NONE, (uint8_t)below(NADDR), 0);
        else if (op < 24) bind(NVMTL_NBUF + NVMTL_NTEX + below(8), below(10) ? SAMP : NONE, (uint8_t)below(NADDR), 0);
        else if (op < 27) { const uint32_t j = below(4) == 0 ? 60 + below(68) : below(8); bind(NVMTL_STEX_BASE + j, below(10) ? TEX : NONE, (uint8_t)below(NADDR), 0); }
        else if (op < 29) {
            /* a texture or sampler destroyed and another made at its address, bound again where the first was */
            const int kind = below(4) ? TEX : SAMP; const uint8_t a = (uint8_t)below(NADDR);
            objgen++; live_at[kind][a] = next_id++; aba++;
            for (uint32_t i = NVMTL_NBUF; i < NVMTL_NBIND; i++) if (slots[i].kind == kind && slots[i].addr == a) bind(i, (uint8_t)kind, a, 0);
        } else if (op < 30) {
            /* a buffer made again at its address: nvmtl_vk_bind_buffer sees the device address change */
            const uint8_t a = (uint8_t)below(NADDR); live_at[BUF][a] = next_id++;
            for (uint32_t i = 0; i < NVMTL_NBUF; i++) if (slots[i].kind == BUF && slots[i].addr == a) mark(i);
        } else if (op < 31) { page_gen++; pages++; }   /* the page filled up: regions are never reused across pages */
        else if (op < 32 || nreg > NREG - 8) new_cb();
        else draw(&pipes[below(NPIPE)], step);
    }
    printf("dbuf_model%s: %ld draws (%ld found an earlier region, %ld new regions, %ld slot changes kept the region, "
           "%ld shadow descriptors made; %ld table-set tables, %ld reused), %ld page changes, %ld destroy+remake at the same "
           "address: %ld FAILED\n", vn[variant], draws, found, written, kept, made, tabs, tabfound, pages, aba, failures);
    free(regions);
    return failures ? 1 : 0;
}
