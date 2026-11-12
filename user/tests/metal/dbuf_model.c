// SPDX-License-Identifier: MIT
// dbuf_model: a host model of the Metal plugin's descriptor-buffer model (nvmtl_db_stage in nullmoth/plugin/nvmtl_dbuf.c)
// that runs the plugin's own decision functions (nullmoth/plugin/nvmtl_dbuf_reuse.h) without Vulkan or a GPU, on any Mac.
// The classic model has the same kind of check in dirty_model.c.
//
// Modelled: binding slots by (kind, pointer, offset) as nvmtl_bind_slot compares them, with the changed bits and dirty
// flag it sets; objects destroyed and made again at the same address (the object generation the plugin bumps); per
// stage layouts (plans) with random slot sets, with or without an address table, read by pipelines that need part or all
// of the table (stage 1d); the per-slot shadow descriptors (made only when not valid); regions written into pages, the
// current region and the MRU of the last four, a page change now and then, and a new command buffer.
// Checked after every draw: the region the stage points at holds, for every slot of its plan, the descriptor of the
// object that slot names now, and its table holds the current buffer addresses and texture sizes of the entries the
// pipeline reads.
//
//   clang -O2 -Wall -o dbuf_model dbuf_model.c && ./dbuf_model [steps=2000000] [seed=1] [variant]
//   variant: keep (default: the plugin), nokeep (NVMTL_DB_KEEP=0), and two deliberately broken ones that must FAIL,
//            to show what the model catches: notable (a kept region's table ignored), noslots (its slots ignored)
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

typedef struct { uint64_t smask[NVMTL_CHG_WORDS]; int table; } plan_t;
static plan_t plans[NPLAN];
typedef struct { int plan; uint32_t need; } pipe_t;
static pipe_t pipes[NPIPE];

/* regions: what was copied into each (offsets are region numbers here) and the page generation it is in */
typedef struct { int64_t d[NVMTL_NBIND]; int64_t tab[NVMTL_NBUF + NVMTL_NTEX]; uint32_t gen; int plan; uint32_t need; } region_t;
static region_t *regions; static uint32_t nreg;
static uint32_t page_gen = 1;
static nvmtl_dbr_stage st;
static int64_t shadow[NVMTL_NBIND];
static int variant;   /* 0 keep, 1 nokeep, 2 notable, 3 noslots */

static long failures, draws, found, kept, written, made, aba, pages;

static void new_cb(void)
{
    memset(&st, 0, sizeof st);
    memset(slots, 0, sizeof slots); memset(chg, 0, sizeof chg); dirty = 0;   /* a pooled binding table is all zero */
    nreg = 0; page_gen = 1;
}

static void check(const pipe_t *p, const region_t *r, long step)
{
    const plan_t *pl = &plans[p->plan];
    long bad = 0; uint32_t first = 0;
    for (uint32_t i = 0; i < NVMTL_NBIND; i++)
        if (((pl->smask[i >> 6] >> (i & 63)) & 1) && r->d[i] != desc_of(i)) { if (!bad++) first = i; }
    if (pl->table) {
        const uint32_t nb = p->need ? (p->need & 0xffu) : NVMTL_NBUF, nt = p->need ? ((p->need >> 8) & 0xffu) : NVMTL_NTEX;
        for (uint32_t b = 0; b < nb; b++) if (r->tab[b] != tab_of(b)) { if (!bad++) first = 1000 + b; }
        for (uint32_t k = 0; k < nt; k++) if (r->tab[NVMTL_NBUF + k] != tab_of(NVMTL_NBUF + k)) { if (!bad++) first = 1000 + NVMTL_NBUF + k; }
    }
    if (r->gen != page_gen) { if (!bad++) first = 9999; }
    if (!bad) return;
    if (failures < 10) fprintf(stderr, "step %ld: %ld stale entr%s, first %s %u\n", step, bad, bad == 1 ? "y" : "ies",
                               first == 9999 ? "page" : first >= 1000 ? "table entry" : "slot", first == 9999 ? 0 : first >= 1000 ? first - 1000 : first);
    failures++;
}

static void draw(const pipe_t *p, long step)
{
    draws++;
    const plan_t *pl = &plans[p->plan];
    if (dirty) {   /* nvmtl_db_stage */
        nvmtl_dbr_region save = st.cur;
        static const uint64_t none[NVMTL_CHG_WORDS];
        if (variant == 2) st.cur.table = 0;      /* broken: a kept region's table is not looked at */
        if (variant == 3) st.cur.smask = none;   /* broken: its slots are not looked at */
        if (nvmtl_dbr_changed(&st, chg, page_gen, variant != 1)) kept++;
        if (variant >= 2) { st.cur.table = save.table; st.cur.smask = save.smask; }
        memset(chg, 0, sizeof chg); dirty = 0;
    }
    nvmtl_dbr_objgen(&st, objgen);
    if (nvmtl_dbr_find(&st, pl, page_gen, p->need)) { found++; check(p, &regions[st.cur.off], step); return; }
    if (nreg == NREG) { fprintf(stderr, "out of regions\n"); exit(2); }
    region_t *r = &regions[nreg];
    memset(r->d, 0xee, sizeof r->d); memset(r->tab, 0xee, sizeof r->tab);
    for (uint32_t i = 0; i < NVMTL_NBIND; i++) {
        if (!((pl->smask[i >> 6] >> (i & 63)) & 1)) continue;
        if (!nvmtl_dbr_valid(&st, i)) { shadow[i] = desc_of(i); nvmtl_dbr_set_valid(&st, i); made++; }
        r->d[i] = shadow[i];
    }
    if (pl->table) {   /* nvmtl_address_snapshot with the stage's need: only those entries */
        const uint32_t nb = p->need ? (p->need & 0xffu) : NVMTL_NBUF, nt = p->need ? ((p->need >> 8) & 0xffu) : NVMTL_NTEX;
        for (uint32_t b = 0; b < nb; b++) r->tab[b] = tab_of(b);
        for (uint32_t k = 0; k < nt; k++) r->tab[NVMTL_NBUF + k] = tab_of(NVMTL_NBUF + k);
    }
    r->gen = page_gen; r->plan = p->plan; r->need = p->need;
    nvmtl_dbr_note(&st, pl, pl->smask, pl->table, nreg, page_gen, p->need);
    nreg++; written++;
    check(p, r, step);
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
        p->table = below(4) != 0;   /* some read no table at all (every entry pushed: table-pc) */
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

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "fmt")) return fmt_check();
    const long steps = argc > 1 ? atol(argv[1]) : 2000000;
    rng = argc > 2 ? strtoull(argv[2], NULL, 10) * 2654435761ull + 1 : 1;
    if (argc > 3) variant = !strcmp(argv[3], "nokeep") ? 1 : !strcmp(argv[3], "notable") ? 2 : !strcmp(argv[3], "noslots") ? 3 : 0;
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
    static const char *vn[] = { "", " (NVMTL_DB_KEEP=0)", " (BROKEN: kept table ignored)", " (BROKEN: kept slots ignored)" };
    printf("dbuf_model%s: %ld draws (%ld found an earlier region, %ld new regions, %ld slot changes kept the region, "
           "%ld shadow descriptors made), %ld page changes, %ld destroy+remake at the same address: %ld FAILED\n",
           vn[variant], draws, found, written, kept, made, pages, aba, failures);
    free(regions);
    return failures ? 1 : 0;
}
