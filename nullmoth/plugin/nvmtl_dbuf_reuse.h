/* SPDX-License-Identifier: MIT */
/* nvmtl_dbuf_reuse.h: when the descriptor-buffer model (nvmtl_dbuf.c) may point a stage at a region of descriptor memory
   it already wrote, and which slot descriptors it has to make again. Pure functions over the stage's bookkeeping, so a
   host program runs the plugin's own decisions without Vulkan or a GPU (user/tests/metal/dbuf_model.c).

   Per command buffer and stage (`which`) the model keeps
     cur      the region the stage points at now: the stage layout (plan) it was written for, where it is (offset in the
              page of generation gen), the stage epoch it is valid at, and the address-table entries it holds (need);
     mru      the last NVMTL_DB_MRU regions written, so a pipeline switch back and forth (A, B, A ...) finds its region;
     epoch    bumped whenever a slot changed or an object was destroyed: a region is reused only at the epoch it was
              noted at;
     valid    the slots whose shadow descriptor (made by vkGetDescriptorEXT) still describes what the slot names;
     objgen   the object generation (nvmtl_objgen) the valid shadows were made at: a destroyed view or sampler may have
              left its pointer to a new object, so every shadow is made again after one.
   2026-11-12 ("keep"): a change that touches nothing the current region holds (no slot of its plan, none of its address
   table's entries) leaves that region valid. Before, any slot change made the next draw write a new region, so a
   pipeline that reads its buffers from pushed table entries (table-pc) still got a fresh region, and ANV a new surface
   state, for every setVertexBytes. NVMTL_DB_KEEP=0 restores that.
   2026-11-13 ("table set"): a stage whose address table has a set of its own (compute's set 1, graphics sets 4 and 5:
   nvmtl_db_table_plan) keeps its table apart from its region (tab): a new table is a new one-descriptor region in that
   set and a new offset, while the stage region (textures, samplers, buffer descriptors) stays where it is. Before, the
   table's descriptor was in the stage region, so every setBuffer offset a table-reading kernel saw wrote a new region.
   2026-11-13 ("bufoff", branch dbufgeek): a kernel's storage buffers whose offsets it takes from push constants
   (nvmtl_bufoff_pc.h) have descriptors that hold the buffer from its offset's 256-byte window (wmask); such a slot's
   descriptor changes only with its wide change (wchg: the buffer, its size, or the offset mod 256), so a new offset alone
   keeps the region. Before, every setBuffer offset of a Geekbench chain changed the descriptor its region held: a new
   region per dispatch whatever the keep rule (13 Nov night, D2b: 1.00).

   The including file defines NVMTL_NBUF, NVMTL_NTEX and NVMTL_CHG_WORDS. */
#ifndef NVMTL_DBUF_REUSE_H
#define NVMTL_DBUF_REUSE_H
#include <stdint.h>
#include <string.h>

#if !defined(NVMTL_NBUF) || !defined(NVMTL_NTEX) || !defined(NVMTL_CHG_WORDS)
#error "nvmtl_dbuf_reuse.h needs NVMTL_NBUF, NVMTL_NTEX and NVMTL_CHG_WORDS"
#endif

#define NVMTL_DB_MRU 4
typedef struct {
    const void *plan;                /* the stage layout the region was written for; NULL = none */
    const uint64_t *smask;           /* NVMTL_CHG_WORDS words: the slots whose descriptors the region holds (the plan's) */
    uint64_t wmask;                  /* buffer slots (word 0) it holds at their offset's window: touched by wchg only */
    uint32_t off, gen, epoch, need;  /* offset in the page of generation gen; the stage epoch it is valid at; its table's
                                        entries (NVMTL_TABNEED form, 0 = the whole table) */
    uint8_t table;                   /* the region holds an address table */
} nvmtl_dbr_region;
/* the table a stage with a table set points at: the table plan it was written for, where it is (offset in the page of
   generation gen) and the entries it holds (NVMTL_TABNEED form, 0 = the whole table). live: no change touched them since. */
typedef struct {
    const void *plan;
    uint32_t off, gen, need;
    uint8_t live;
} nvmtl_dbr_tab;
typedef struct {
    nvmtl_dbr_region cur, mru[NVMTL_DB_MRU];
    nvmtl_dbr_tab tab;
    uint32_t epoch, mru_next;
    uint64_t objgen;
    uint64_t valid[NVMTL_CHG_WORDS];
    uint64_t wvalid;                 /* buffer slots whose window shadow (NVMTL_DBT_SSBOW) still holds: cleared by wchg */
} nvmtl_dbr_stage;

/* a table holding entries `have` serves a stage that reads `need` (0 = the whole table), as nvmtl_tabneed_covers */
static inline int nvmtl_dbr_covers(uint32_t have, uint32_t need)
{
    if (!have) return 1;
    if (!need) return 0;
    return (have & 0xffu) >= (need & 0xffu) && ((have >> 8) & 0xffu) >= ((need >> 8) & 0xffu);
}
/* the bits of [lo, hi) that fall in word k */
static inline uint64_t nvmtl_dbr_range_word(uint32_t lo, uint32_t hi, uint32_t k)
{
    const uint32_t b0 = k * 64, b1 = b0 + 64;
    if (hi <= b0 || lo >= b1 || lo >= hi) return 0;
    const uint32_t a = lo > b0 ? lo - b0 : 0, z = hi < b1 ? hi - b0 : 64;
    const uint64_t upto = z >= 64 ? ~0ull : (1ull << z) - 1;
    return upto & ~((1ull << a) - 1);
}
/* word k of the slots a table holding entries `need` is made from: the buffer addresses (slots 0..) and texture sizes
   (slots NVMTL_NBUF..) */
static inline uint64_t nvmtl_dbr_tab_word(uint32_t need, uint32_t k)
{
    const uint32_t nb = need ? (need & 0xffu) : NVMTL_NBUF, nt = need ? ((need >> 8) & 0xffu) : NVMTL_NTEX;
    return nvmtl_dbr_range_word(0, nb, k) | nvmtl_dbr_range_word(NVMTL_NBUF, NVMTL_NBUF + nt, k);
}
/* does a change of the slots in chg (wide changes in wchg) alter what region r holds? Its descriptors (smask; the
   window descriptors of wmask by wchg), and, with a table, the entries its table was written with */
static inline int nvmtl_dbr_touches(const nvmtl_dbr_region *r, const uint64_t *chg, uint64_t wchg)
{
    if (wchg & r->wmask) return 1;
    for (uint32_t k = 0; k < NVMTL_CHG_WORDS; k++) {
        uint64_t m = r->smask ? r->smask[k] : ~0ull;
        if (r->table) m |= nvmtl_dbr_tab_word(r->need, k);
        if (chg[k] & m) return 1;
    }
    return 0;
}
/* does a change of the slots in chg alter a table holding entries `need`? */
static inline int nvmtl_dbr_tab_touches(uint32_t need, const uint64_t *chg)
{
    for (uint32_t k = 0; k < NVMTL_CHG_WORDS; k++) if (chg[k] & nvmtl_dbr_tab_word(need, k)) return 1;
    return 0;
}
/* slots in chg changed. Their shadows are made again when a plan next reads them; regions noted before are stale
   (epoch), except, with keep on, those the change touches nothing of. 1 = the current region was kept. A region is kept
   only while it is in the bound page (gen_now): reuse never crosses a page.
   keep_on 1 (NVMTL_DB_KEEP=cur, the rule of 2026-11-12): only the current region is looked at.
   keep_on 2 (default, 2026-11-13): every recent region valid until now is looked at. A compute chain A, B, C, A ... with
   a setBuffer or setBytes between the dispatches killed the regions of B and C on every change even when none of them
   read the changed slot (a pushed entry: cs-table-pc), so every dispatch wrote a new region (Geekbench 6 Metal: 0.97
   regions per bind).
   The stage's own table (table set) dies when the change touches an entry it holds; with keep off, on any change. */
static inline int nvmtl_dbr_changed(nvmtl_dbr_stage *st, const uint64_t *chg, uint64_t wchg, uint32_t gen_now, int keep_on)
{
    const uint32_t was = st->epoch;
    if (st->tab.live && (!keep_on || nvmtl_dbr_tab_touches(st->tab.need, chg))) st->tab.live = 0;
    const int keep = keep_on && st->cur.plan && st->cur.gen == gen_now && st->cur.epoch == was && !nvmtl_dbr_touches(&st->cur, chg, wchg);
    for (uint32_t k = 0; k < NVMTL_CHG_WORDS; k++) st->valid[k] &= ~chg[k];
    st->wvalid &= ~wchg;
    st->epoch++;
    if (!keep_on) return 0;
    for (uint32_t m = 0; m < NVMTL_DB_MRU; m++) {
        nvmtl_dbr_region *r = &st->mru[m];
        if (!r->plan || r->gen != gen_now || r->epoch != was) continue;
        const int same = keep && r->plan == st->cur.plan && r->off == st->cur.off;
        if (same || (keep_on >= 2 && !nvmtl_dbr_touches(r, chg, wchg))) r->epoch = st->epoch;
    }
    if (!keep) return 0;
    st->cur.epoch = st->epoch;
    return 1;
}
/* a view, sampler or heap slot was given up since the shadows were made: none of them is trusted, no region either */
static inline void nvmtl_dbr_objgen(nvmtl_dbr_stage *st, uint64_t gen)
{
    if (st->objgen == gen) return;
    memset(st->valid, 0, sizeof st->valid); st->wvalid = 0; st->objgen = gen; st->epoch++; st->tab.live = 0;
}
/* a region written for plan in page gen that serves a stage reading table entries need: the current one or one of the
   last few. 1 = found (st->cur is it). */
static inline int nvmtl_dbr_find(nvmtl_dbr_stage *st, const void *plan, uint32_t gen, uint32_t need)
{
    if (st->cur.plan == plan && st->cur.gen == gen && st->cur.epoch == st->epoch && nvmtl_dbr_covers(st->cur.need, need)) return 1;
    for (uint32_t m = 0; m < NVMTL_DB_MRU; m++) {
        const nvmtl_dbr_region *r = &st->mru[m];
        if (r->plan == plan && r->gen == gen && r->epoch == st->epoch && nvmtl_dbr_covers(r->need, need)) { st->cur = *r; return 1; }
    }
    return 0;
}
/* a new region was written */
static inline void nvmtl_dbr_note(nvmtl_dbr_stage *st, const void *plan, const uint64_t *smask, uint64_t wmask, int table, uint32_t off,
                                  uint32_t gen, uint32_t need)
{
    st->cur = (nvmtl_dbr_region){ plan, smask, wmask, off, gen, st->epoch, need, (uint8_t)(table != 0) };
    st->mru[st->mru_next++ % NVMTL_DB_MRU] = st->cur;
}
/* the stage's table serves a stage with table plan `plan` reading entries `need` in page gen: 1 = point the set at
   st->tab.off again */
static inline int nvmtl_dbr_tab_find(const nvmtl_dbr_stage *st, const void *plan, uint32_t gen, uint32_t need)
{
    return st->tab.live && st->tab.plan == plan && st->tab.gen == gen && nvmtl_dbr_covers(st->tab.need, need);
}
/* a new table was written (its descriptor at off, in a region of table plan `plan`) */
static inline void nvmtl_dbr_tab_note(nvmtl_dbr_stage *st, const void *plan, uint32_t off, uint32_t gen, uint32_t need)
{
    st->tab = (nvmtl_dbr_tab){ plan, off, gen, need, 1 };
}
static inline int nvmtl_dbr_valid(const nvmtl_dbr_stage *st, uint32_t i) { return (int)((st->valid[i >> 6] >> (i & 63)) & 1); }
static inline void nvmtl_dbr_set_valid(nvmtl_dbr_stage *st, uint32_t i) { st->valid[i >> 6] |= 1ull << (i & 63); }
/* the shadow of slot i (< 64) made as a window descriptor: wide (1) or exact (0). A window shadow stays good across
   offset-only changes, an exact one does not */
static inline int nvmtl_dbr_shadow_ok(const nvmtl_dbr_stage *st, uint32_t i, int wide)
{
    return wide ? (int)((st->wvalid >> i) & 1) : nvmtl_dbr_valid(st, i);
}
static inline void nvmtl_dbr_shadow_made(nvmtl_dbr_stage *st, uint32_t i, int wide)
{
    if (wide) st->wvalid |= 1ull << i; else nvmtl_dbr_set_valid(st, i);
}
#endif
