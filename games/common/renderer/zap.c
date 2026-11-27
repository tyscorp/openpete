#include "openpete_thread_roles.h"
// games/common/renderer/zap.c, the feeder for Zoe's zap: a handwritten routine
// (Spyro 2 func_8004B194 / mobyfunc.s:2548-2905, Spyro 3 func_8002F5B4 /
// mobyfunc.s:2548-2936) that draws one ragged textured ribbon per live record
// of a pool. The records are spawned by the level overlays' Zoe update through
// the spawner beside it (Spyro 3 func_8002F540 / asm/mobyfunc.s:2511-2544,
// Spyro 2 func_8004B120 / mobyfunc.s:2510-2543), which is why the pool is empty
// everywhere Zoe is not zapping. The routine links every triangle into the 3D
// ordering table itself, so none of its primitives reaches the 2D chokepoint
// the sprite feeder hooks. Spyro
// 3's routine carries seven instructions Spyro 2's lacks, a game-state test
// that suppresses the per-present re-randomize; everything this feeder reads is
// in the 358 instructions the two share. Every per-game address, count, stride
// and bound comes from game_layout.zap; the walk below is one body.
//
// Read from the routine (asm/mobyfunc.s):
//   * the pool is 8 records of 0x30 bytes: the head world XYZ at +0x00, the
//     tail world XYZ at +0x0C, the primitive colour word at +0x18 and a packed
//     word at +0x1C holding the segment count in byte 0, the jitter shift in
//     byte 1 and a re-randomize flag in the upper halfword (:2588-2593). One
//     stored random halfword per interior point follows at +0x20. A zero count
//     byte is a free slot; the spawner fills the halfwords from rand and forces
//     the colour word's top byte to 0x26, POLY_FT3 semi-transparent
//     (:2512-2543).
//   * the camera matrix comes from G_CAMERA+0x00 with the GTE translation
//     registers zeroed (:2564-2578), so every MAC the routine computes is a
//     plain rotation of a camera-relative offset in full-world units.
//   * the record is dropped unless each component of the head's camera-relative
//     offset stays inside a +/-0x5400 box (:2608-2628); the tail is not tested.
//   * the routine lays down count+1 points from the head to the tail by
//     repeated addition of the integer-divided step (:2638-2648, :2652-2657),
//     displacing each interior point by the three signed 8-bit fields packed in
//     that point's stored random halfword, arithmetic-shifted right by the
//     jitter shift (:2658-2672). With the shift non-zero and the game not in
//     state 4 it draws a fresh rand, stores it back over the halfword and adds
//     its fields at half that scale as well (:2673-2685). Both displacements
//     are bounded by 192 world units against the 0x5400 box, so the depth this
//     feeder derives from the undisplaced chain is the routine's own chain to
//     within that bound.
//   * every point is then widened into a pair offset perpendicular to the local
//     path direction by about a sixteenth of the distance between its
//     neighbours (:2693-2745); the head and the tail are written as coincident
//     pairs instead (:2747-2766), which is why the first and last segment emit
//     one triangle and every interior segment two.
//   * each of the pair entries is RTPSed into the scratchpad as {packed screen
//     XY, SZ3 in the upper half and a four-bit outcode in the lower}
//     (:2773-2803); a record whose every entry shares an outcode bit lies
//     wholly off one screen edge and is dropped (:2804-2806).
//   * the record's depth key is the SZ3 of its middle point (:2821-2827): at
//     0x5000 and beyond the record is dropped, and from 0x4000 up a DPCS fades
//     the record colour toward black by (SZ3 - 0x4000) / 4096 (:2828-2836), so
//     the colour word this feeder reads back already carries that fade.
//   * per segment the four corner outcodes are ANDed and the segment skipped
//     when any bit survives (:2843-2848), then the ordering-table bucket is
//     ((sum of the four SZ3) >> 9) - 2 entries of eight bytes off the base
//     (:2849-2860), so both triangles of a segment sit at one depth.
//
// The geometry is the guest's own int16 output, read back out of the polybuf,
// not a second projection. What the primitives do not carry is depth - the PSX
// orders them by ordering-table bucket - so each segment's view-Z comes from
// re-running the routine for every live record in slot order: its integer
// chain, its widening and its RTPS of every entry, into a private entry table,
// then its emit loop over that table (the per-segment outcode AND, the
// four-corner SZ3 sum and the two coincidence tests). The scratchpad table
// holds only the last record the routine projected, so it cannot serve a call
// with two or more; it calibrates the re-run instead. The interior points'
// random displacement is reproducible after the call: the old halfword comes
// from a snapshot of the pool taken before the call, the fresh one is the
// halfword the routine stored back. The replayed triangles pair to the captured
// primitives in emit order when the counts agree; otherwise each primitive
// takes the depth of the replayed triangle whose corners lie nearest its own,
// or with none replayed the depth of the nearest live record's undisplaced
// middle chain point, and the census counts it.
//
// This is not a second branch of the gem-glint family in glint.c: that pool is
// 16 records of 0x10 bytes drawn as a screen-facing star of eight POLY_G4 over
// a halved camera offset, this one is 8 records of 0x30 bytes drawn as a
// textured POLY_FT3 strip between two world points in full-world units, and the
// two share no record field, no primitive class and no reject. What they do
// share - the hook bracket, the polybuf walk, the primitive decode and the
// projection helpers - is already shared and is reused here unchanged.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the random halfwords
// and the alternating jitter shift it stores into each record; this feeder only
// reads them afterwards.

#include "openpete_fiber.h"  /* openpete_feed_key_now: instrument line labels */
#include "zap.h"
#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/cut_guard.h"     /* cut_held_sets_t: the held cut's tick N-1 set */
#include "renderer/env_knob.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_interp_objs.h"   /* the per-record prev/curr pool snapshots */
#include "game_layout.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

#define ZAP (game_layout.zap)

#define ZAP_MAX_PRIMS 512
#define ZAP_MAX_SEG   128

/* Where a triangle corner's chain point projects: the midpoint of the two
 * entries the routine widened that point into, which is the point the corner
 * is offset from by the ribbon's half-width. `ok` is 0 for a corner of the
 * head or the tail, which the routine writes as a coincident pair and so
 * carries no width. */
typedef struct {
    float   x[3], y[3];
    uint8_t ok[3];
    uint8_t pt[3];   /* the chain point's index, 0 at the head */
} zap_pivot;

typedef struct {
    ngte_prim p;
    float     depth;
    int       paired;
    zap_pivot pv;
} zap_item;

static zap_item  s_item[ZAP_MAX_PRIMS];
static int      s_item_n;
static float    s_pred[ZAP_MAX_PRIMS];   /* one depth per predicted triangle */
static zap_pivot s_pred_pv[ZAP_MAX_PRIMS]; /* its corners' chain points */
static int      s_pred_n;
static int16_t  s_pred_xy[ZAP_MAX_PRIMS][3][2] OP_GUARDED_BY(sim); /* its corners as the routine emits them */
static int      s_live;                 /* records with a non-zero count byte */
static int      s_accepted;             /* 1 when the replay accounted for the call */
static int      s_reached;              /* records that reached the projection pass */
static int      s_paired_recs OP_GUARDED_BY(sim);          /* records whose replayed triangles paired */
static int      s_fallback OP_GUARDED_BY(sim);             /* primitives paired by nearest triangle or record */
static int      s_corner_bad OP_GUARDED_BY(sim);           /* paired primitives whose corners differ from the replay */
static int      s_cal_words OP_GUARDED_BY(sim);            /* scratchpad words compared against the replay */
static int      s_cal_bad OP_GUARDED_BY(sim);              /* of those, the words that differ */
static uint32_t s_code_hist[256];
static uint32_t s_tag_hist[256];
static uint32_t s_fed_tris;

/* The record the pairing accepted, and where the routine's own projection puts
 * its head and tail on screen at the tick. The ribbon runs from one to the
 * other, so these two points are what a sub-tick present has to move it by.
 * `s_rec` is 0 when the call paired no record. */
static uint32_t s_rec;
static uint32_t s_head_w[3], s_tail_w[3];
static ngte_proj s_head_p, s_tail_p;
static int      s_seg_n;                     /* the record's segment count */
static double   s_hw_tick[ZAP_MAX_SEG + 1];  /* each chain point's screen half-width */

/* The sub-tick rebase: each end's screen displacement and re-projected view-Z.
 * Inactive on a canonical feed and at the end of the tick's own interval,
 * where the feed must reproduce the canonical primitive bit for bit. */
static struct {
    double dhx, dhy, dtx, dty;   /* head and tail screen displacement */
    double head_m3, tail_m3;     /* their re-projected view-Z */
    double hw_scale[ZAP_MAX_SEG + 1];  /* each chain point's half-width, shown over the tick's */
    int    active;
} s_reb;

/* The tick's captured set, replayed by every sub-tick present of the tick. */
typedef struct { ngte_prim p; float depth; zap_pivot pv; } zap_held;
static zap_held  s_tick_buf[2][ZAP_MAX_PRIMS];
static zap_held* s_tick = s_tick_buf[0];
static int      s_tick_n;
/* The set the last capture of a new canonical epoch retired, which a held
 * camera cut shows when it is tick N-1's. */
static zap_held* s_tick_old = s_tick_buf[1];
static int      s_tick_old_n;

/** @brief Exchange the tick's held set with the retired one, with their counts. */
static void zap_sets_swap(void) {
    zap_held* t = s_tick; s_tick = s_tick_old; s_tick_old = t;
    const int n = s_tick_n; s_tick_n = s_tick_old_n; s_tick_old_n = n;
}

static cut_held_sets_t s_sets = { zap_sets_swap, &s_tick_n, "zap", { 0u, 0u }, 0u };

/** @brief Stores one decoded primitive in s_item as an unpaired item at depth 1 until zap_pair_depths places it.
 *
 * @param ctx Unused.
 * @param s The decoded primitive.
 * @return 1 when stored, 0 when s_item holds ZAP_MAX_PRIMS items. */
static int zap_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= ZAP_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 1.0f;   /* replaced by zap_pair_depths */
    s_item[s_item_n].paired = 0;
    memset(&s_item[s_item_n].pv, 0, sizeof s_item[0].pv);
    s_item_n++;
    return 1;
}

/** @brief Decodes every primitive in the polybuf interval the zap routine's call allocated into s_item.
 *
 * The routine emits 8-word POLY_FT3 (tag length 7); the walk admits tags up
 * to that length, up to ZAP_MAX_PRIMS primitives, and fills the code and tag
 * histograms. Every captured primitive starts unpaired at depth 1.
 * @param cpu The guest CPU state.
 * @param start The polybuf cursor before the call.
 * @param end The polybuf cursor after the call. */
static void zap_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 7, .raw_words_max = 8, .guard = ZAP_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, zap_prim_sink, NULL);
}

/* The pool as it stood before the call: the routine overwrites each interior
 * point's random halfword and flips the jitter shift byte, so the re-run reads
 * the record's own inputs from here and only the fresh halfwords from RAM. */
#define ZAP_MAX_SLOTS 16
#define ZAP_PRE_WORDS 256
static uint32_t s_pre[ZAP_PRE_WORDS] OP_GUARDED_BY(sim);
static uint32_t s_pre_bytes OP_GUARDED_BY(sim);   /* bytes of the pool the snapshot covers, 0 when none */

/** @brief Copies the pool into s_pre before the routine runs.
 *
 * Covers ZAP.slots records of ZAP.stride bytes, capped at ZAP_PRE_WORDS words.
 * @param cpu The guest CPU state, read only. */
static void zap_snapshot(CPUState* cpu) OP_REQUIRES(sim) {
    uint32_t bytes = ZAP.slots * ZAP.stride;
    if (bytes > ZAP_PRE_WORDS * 4u) bytes = ZAP_PRE_WORDS * 4u;
    for (uint32_t w = 0; w < bytes / 4u; w++) s_pre[w] = cpu->read_word(ZAP.pool + w * 4u);
    s_pre_bytes = bytes & ~3u;
}

/** @brief Reports whether the snapshot covers the pool bytes [addr, addr + len).
 * @param addr The first byte.
 * @param len The byte count.
 * @return 1 when covered, else 0. */
static int zap_pre_covers(uint32_t addr, uint32_t len) OP_REQUIRES_SHARED(sim) {
    return addr >= ZAP.pool && addr + len <= ZAP.pool + s_pre_bytes;
}

/** @brief Reads one word of the pool as it stood before the call.
 * @param addr A word-aligned address the snapshot covers.
 * @return The word. */
static uint32_t zap_pre_word(uint32_t addr) OP_REQUIRES_SHARED(sim) {
    return s_pre[(addr - ZAP.pool) / 4u];
}

/** @brief Reads one halfword of the pool as it stood before the call, as lhu loads it.
 * @param addr A halfword-aligned address the snapshot covers.
 * @return The halfword, zero-extended. */
static uint32_t zap_pre_half(uint32_t addr) OP_REQUIRES_SHARED(sim) {
    return (zap_pre_word(addr & ~3u) >> ((addr & 2u) * 8u)) & 0xFFFFu;
}

/** One call's inputs to the re-run: the GTE state the routine loads and the
 * staged camera position it subtracts (:2564-2578). */
typedef struct {
    int     M[3][3];
    int32_t cam[3];
    double  ofx, ofy;
    int     H;
    int32_t state;   /* game_state, read when ZAP.rerand_hold >= 0 */
} zap_emu_ctx;

/** @brief Packs two halfwords into one word, the first in the low half, as two sh store them.
 * @param lo The low halfword.
 * @param hi The high halfword.
 * @return The word. */
static uint32_t zap_pack(int32_t lo, int32_t hi) {
    return ((uint32_t)lo & 0xFFFFu) | (((uint32_t)hi & 0xFFFFu) << 16);
}

/** @brief Projects one entry of the table in place as the routine's RTPS pass does (:2773-2803).
 *
 * VXY0 is word 0, VZ0 the low half of word 1; under the zero translation the
 * result is word 0 = SXY2 and word 1 = SZ3 in the upper half with the four
 * outcode bits below: 1 for SXY2 at or below 0xD0000 (y <= 13), 2 for SXY2 at
 * or above 0xE40000 (y >= 228), 4 for x <= 0 and 8 for x >= 512, each a
 * signed word comparison on the packed value.
 * @param c The call's GTE state.
 * @param e The entry, rewritten. */
static void zap_emu_rtps(const zap_emu_ctx* c, uint32_t e[2]) {
    static const int k_tr0[3] = { 0, 0, 0 };
    ngte_rtps_out o;
    ngte_rtps_int(c->M, k_tr0, (int16_t)(e[0] & 0xFFFFu), (int16_t)(e[0] >> 16),
                  (int16_t)(e[1] & 0xFFFFu), c->ofx, c->ofy, c->H, &o);
    const uint32_t at = zap_pack(o.sx, o.sy);
    uint32_t v0 = (uint32_t)o.sz3 << 16;
    if (!((int32_t)(at - 0xD0000u) > 0)) v0 += 1u;
    if (!((int32_t)(at - 0xE40000u) < 0)) v0 += 2u;
    if (!((int32_t)(at << 16) > 0)) v0 += 4u;
    if (!((int32_t)((at << 16) - 0x2000000u) < 0)) v0 += 8u;
    e[0] = at;
    e[1] = v0;
}

/** @brief Lays down one record's displaced point chain as the routine does (:2638-2685).
 *
 * Point 0 is the head's camera-relative offset and point n the tail's; the
 * step is the integer quotient of their difference by n, accumulated from the
 * head. Interior point i adds the three signed 8-bit fields of its old random
 * halfword (bits 14:7, 9:2 and 4:0 << 3) shifted right by the jitter shift,
 * and, when the routine re-randomized, the fields of the fresh halfword it
 * stored, shifted right one more. Spyro 3 skips the re-randomize while
 * game_state equals ZAP.rerand_hold, and stops it for the rest of the record
 * once a fresh value equals its own third field (the beq of $v0 against $at,
 * asm/mobyfunc.s:2671), which only a fresh value of 0 does.
 * @param cpu The guest CPU state, read for the fresh halfwords.
 * @param c The call's inputs.
 * @param rec The record's address.
 * @param n The segment count.
 * @param P Receives points 0 to n, camera-relative.
 * @return 1 when laid down, 0 when the head leaves the +/-ZAP.box cube, -1
 *         when the record's inputs fall outside the snapshot or Spyro 3's
 *         compare reads registers a flagless record never loaded. */
static int zap_emu_chain(CPUState* cpu, const zap_emu_ctx* c, uint32_t rec, int n,
                         int32_t P[][3]) OP_REQUIRES_SHARED(sim) {
    const uint32_t halves = rec + ZAP.packed_off + 4u;   /* :2512-2543 the spawner's halfwords */
    if (!zap_pre_covers(rec + ZAP.head_off, 24u) || !zap_pre_covers(rec + ZAP.packed_off, 4u) ||
        !zap_pre_covers(halves, 2u * (uint32_t)(n - 1))) return -1;
    const uint32_t packed = zap_pre_word(rec + ZAP.packed_off);
    const int sh = (int)((packed >> 8) & 0xFFu) & 31;
    for (int k = 0; k < 3; k++) {
        const int32_t h = (int32_t)zap_pre_word(rec + ZAP.head_off + 4u * (uint32_t)k);
        const int32_t t = (int32_t)zap_pre_word(rec + ZAP.head_off + 12u + 4u * (uint32_t)k);
        P[0][k] = (k == 0) ? h - c->cam[0] : c->cam[k] - h;
        P[n][k] = (k == 0) ? t - c->cam[0] : c->cam[k] - t;
        if (P[0][k] - ZAP.box >= 0 || P[0][k] + ZAP.box <= 0) return 0;   /* :2608-2628 */
    }
    const int s3 = ZAP.rerand_hold >= 0;
    if (s3 && sh != 0 && (packed >> 16) == 0) return -1;
    int32_t cv = c->state, ca = ZAP.rerand_hold;
    int32_t step[3], acc[3];
    for (int k = 0; k < 3; k++) { step[k] = (P[n][k] - P[0][k]) / n; acc[k] = P[0][k]; }
    static const int k_sl[3] = { 17, 22, 27 };
    for (int i = 1; i < n; i++) {
        const uint32_t ha = halves + 2u * (uint32_t)(i - 1);
        const uint32_t old = zap_pre_half(ha);
        for (int k = 0; k < 3; k++) {
            acc[k] += step[k];
            P[i][k] = acc[k] + (((int32_t)(old << k_sl[k]) >> 24) >> sh);
        }
        if (sh == 0 || (s3 && cv == ca)) continue;
        const uint32_t r = (uint32_t)cpu->read_half(ha);
        for (int k = 0; k < 3; k++) P[i][k] += (int32_t)(r << k_sl[k]) >> 25;
        cv = (int32_t)r;
        ca = (int32_t)(r << 27) >> 25;
    }
    return 1;
}

/** @brief Re-runs the routine for one record into a private entry table: chain, widening and projection.
 *
 * Interior point i is widened (:2693-2745) by d, its neighbours' absolute
 * difference: w = (max(|dx|, |dy|) + min(|dx|, |dy|) / 2) / 16 on VY, and
 * |dz| / 16 on VX where |dx| >= |dy|, else on VZ, the two entries taking the
 * plus and minus sides. An entry is VX = point Y, VY = point Z, VZ = point X,
 * the halves truncated as sh stores them; the head and the tail are written
 * as coincident pairs (:2747-2766). Every entry 0 to 2n+1 is then projected.
 * @param cpu The guest CPU state.
 * @param c The call's inputs.
 * @param rec The record's address.
 * @param n The segment count, 1 to ZAP_MAX_SEG.
 * @param tab Receives 2n+2 entries of two words.
 * @return As zap_emu_chain. */
static int zap_emu_table(CPUState* cpu, const zap_emu_ctx* c, uint32_t rec, int n,
                         uint32_t tab[][2]) OP_REQUIRES_SHARED(sim) {
    int32_t P[ZAP_MAX_SEG + 1][3];
    const int r = zap_emu_chain(cpu, c, rec, n, P);
    if (r <= 0) return r;
    for (int i = 0; i <= n; i += n) {
        tab[2 * i][0] = tab[2 * i + 1][0] = zap_pack(P[i][1], P[i][2]);
        tab[2 * i][1] = tab[2 * i + 1][1] = (uint32_t)P[i][0];
    }
    for (int i = 1; i < n; i++) {
        int32_t d[3];
        for (int k = 0; k < 3; k++) {
            d[k] = P[i + 1][k] - P[i - 1][k];
            if (d[k] < 0) d[k] = -d[k];
        }
        const int xwide = d[0] - d[1] >= 0;
        const int32_t hi = xwide ? d[0] : d[1], lo = xwide ? d[1] : d[0];
        const int32_t w = (int32_t)(((uint32_t)hi + ((uint32_t)lo >> 1)) >> 4);
        const int32_t dz = d[2] >> 4;
        const int32_t ax = P[i][1] + (xwide ? dz : 0), bx = P[i][1] - (xwide ? dz : 0);
        const int32_t az = P[i][0] + (xwide ? 0 : dz), bz = P[i][0] - (xwide ? 0 : dz);
        tab[2 * i][0] = zap_pack(ax, P[i][2] + w);
        tab[2 * i][1] = (uint32_t)az;
        tab[2 * i + 1][0] = zap_pack(bx, P[i][2] - w);
        tab[2 * i + 1][1] = (uint32_t)bz;
    }
    for (int e = 0; e <= 2 * n + 1; e++) zap_emu_rtps(c, tab[e]);
    return 1;
}

/** @brief Compares a replayed entry table with the one the routine left in the scratchpad.
 *
 * The scratchpad holds the last record that passed the box test, so this is
 * the re-run's calibration: every word must agree. Adds to s_cal_words and
 * s_cal_bad.
 * @param cpu The guest CPU state.
 * @param tab The replayed table.
 * @param n That record's segment count. */
static void zap_emu_calibrate(CPUState* cpu, const uint32_t tab[][2], int n) OP_REQUIRES(sim) {
    for (int e = 0; e <= 2 * n + 1; e++)
        for (int w = 0; w < 2; w++) {
            s_cal_words++;
            if (cpu->read_word(ZAP.spad + (uint32_t)e * 8u + 4u * (uint32_t)w) != tab[e][w]) s_cal_bad++;
        }
}

/** @brief Reads a record's head and tail world points.
 *
 * The head is at ZAP.head_off and the tail twelve bytes on, both three
 * signed words in full world units, returned as raw words.
 * @param cpu The guest CPU state.
 * @param rec The record's address.
 * @param head_w Receives the head's three words.
 * @param tail_w Receives the tail's three words. */
static void zap_rec_points(CPUState* cpu, uint32_t rec,
                           uint32_t head_w[3], uint32_t tail_w[3]) {
    for (int k = 0; k < 3; k++) {
        head_w[k] = cpu->read_word(rec + ZAP.head_off + (uint32_t)k * 4u);
        tail_w[k] = cpu->read_word(rec + ZAP.head_off + 12u + (uint32_t)k * 4u);
    }
}

/** @brief Projects one record world point through the routine's own arithmetic.
 *
 * The camera-relative offset the routine subtracts (:2564-2578 zero the GTE
 * translation, so every MAC is a plain rotation of a camera-relative offset
 * in full world units) is rotated and divided by ngte_project_offset.
 * @param v The view to project under.
 * @param w The point's three world words.
 * @param off A displacement in full world units, or NULL for none.
 * @param o Receives the projection. */
static void zap_project_point(const ngte_view* v, const uint32_t w[3],
                              const double off[3], ngte_proj* o) {
    double q[3];
    for (int k = 0; k < 3; k++) q[k] = (double)(int32_t)w[k] + (off ? off[k] : 0.0);
    const double rel[3] = { q[0] - v->cam[0], v->cam[1] - q[1], v->cam[2] - q[2] };
    ngte_project_offset(v, rel, o);
}

/** @brief Computes the screen half-width of chain point i of n with the head and tail world points displaced.
 *
 * The routine lays the undisplaced point at head + i * step (:2638-2657) and
 * widens it in camera-relative world units, before the projection, from the
 * difference d between its two neighbours (:2693-2745): a vertical offset
 * of max(|dx|, |dy|) + min(|dx|, |dy|) / 2 and a horizontal offset of |dz|
 * along world X where |dx| < |dy| and along world Y otherwise, both shifted
 * right by four, the pair's two entries sitting at the point plus and minus
 * that vector. The neighbours are jittered points (:2658-2685), so the world
 * offset is the tick's and is held: it is built from the tick's undisplaced
 * head-to-tail vector, of which the step is a fixed fraction, and only the
 * point it is carried on is displaced. What varies is the offset's
 * projection, its view-Z and its orientation to the camera.
 * @param v The view to project under.
 * @param oh The head's displacement in full world units.
 * @param ot The tail's displacement in full world units.
 * @param i The chain point's index, 0 at the head.
 * @param n The segment count.
 * @return Half the screen distance between the two entries' projections, or
 *         0 when either lies behind the camera. */
static double zap_half_width(const ngte_view* v, const double oh[3], const double ot[3],
                             int i, int n) {
    double d[3], p[3], o[3], q[3];
    const double f = (double)i / (double)n;
    for (int k = 0; k < 3; k++) {
        const double h = (double)(int32_t)s_head_w[k];
        d[k] = (double)(int32_t)s_tail_w[k] - h;
        p[k] = h + oh[k] + (d[k] + ot[k] - oh[k]) * f;
    }
    /* The neighbours' difference is two steps of d / n. */
    const double sc = 2.0 / (double)n / 16.0;
    const double ax = fabs(d[0]) * sc, ay = fabs(d[1]) * sc, az = fabs(d[2]) * sc;
    o[0] = (ax < ay) ? az : 0.0;
    o[1] = (ax < ay) ? 0.0 : -az;
    o[2] = -(fmax(ax, ay) + fmin(ax, ay) * 0.5);
    ngte_proj a, b;
    for (int k = 0; k < 3; k++) q[k] = p[k] + o[k];
    const double ra[3] = { q[0] - v->cam[0], v->cam[1] - q[1], v->cam[2] - q[2] };
    for (int k = 0; k < 3; k++) q[k] = p[k] - o[k];
    const double rb[3] = { q[0] - v->cam[0], v->cam[1] - q[1], v->cam[2] - q[2] };
    ngte_project_offset(v, ra, &a);
    ngte_project_offset(v, rb, &b);
    if (!(a.m3 > 0.0) || !(b.m3 > 0.0)) return 0.0;
    return 0.5 * hypot(a.sx - b.sx, a.sy - b.sy);
}

/* The band is the discontinuity guard ZAP_REUSE_CEILING gives the
 * displacement: a half-width that changed by more than a factor of four
 * inside one tick is a camera cut or a slot reuse, where the replay holds the
 * tick's width as every other term of the rebase does. */
#define ZAP_SCALE_MIN 0.25
#define ZAP_SCALE_MAX 4.0

/** @brief Reports whether the sub-tick replay scales the ribbon's width, latched from OPENPETE_ZAP_WIDTH on first call.
 *
 * OPENPETE_ZAP_WIDTH=0 holds the tick's width on the replay, the engagement
 * leg of the sub-tick width harness; any other value or none enables it.
 * @return 1 when enabled, else 0. */
static int zap_width_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_ZAP_WIDTH"); v = !(e && *e == '0'); }
    return v;
}

/** @brief Computes each interior chain point's half-width scale into s_reb.hw_scale.
 *
 * The scale is the half-width this present shows over the one the tick drew,
 * both from zap_half_width. The head, the tail, a point whose tick half-width
 * is not positive, and a scale outside [ZAP_SCALE_MIN, ZAP_SCALE_MAX] (which
 * covers a point projecting behind the camera, whose half-width is 0) hold
 * the tick's width at scale 1, as does every point when zap_width_enabled is 0.
 * @param v The view this present shows.
 * @param oh The head's displacement in full world units.
 * @param ot The tail's displacement in full world units. */
static void zap_width_scales(const ngte_view* v, const double oh[3], const double ot[3]) {
    const int on = zap_width_enabled();
    for (int i = 0; i <= s_seg_n; i++) {
        s_reb.hw_scale[i] = 1.0;
        if (!on || i == 0 || i == s_seg_n || !(s_hw_tick[i] > 0.0)) continue;
        const double scl = zap_half_width(v, oh, ot, i, s_seg_n) / s_hw_tick[i];
        if (scl >= ZAP_SCALE_MIN && scl <= ZAP_SCALE_MAX) s_reb.hw_scale[i] = scl;
    }
}

/** @brief Computes the midpoint of two packed screen-XY entries.
 *
 * @param a The first entry (x in the low signed halfword, y in the high).
 * @param b The second entry.
 * @param x Receives the midpoint's x.
 * @param y Receives the midpoint's y. */
static void zap_entry_mid(uint32_t a, uint32_t b, float* x, float* y) {
    *x = ((float)(int16_t)(a & 0xFFFFu) + (float)(int16_t)(b & 0xFFFFu)) * 0.5f;
    *y = ((float)(int16_t)(a >> 16) + (float)(int16_t)(b >> 16)) * 0.5f;
}

/** @brief Appends one predicted triangle of segment j with its depth, its corners and their chain points.
 *
 * Segment j joins points j-1 and j of n. Its four entries are point j-1's
 * pair (0, 1) and point j's pair (2, 3); the first triangle's corners are
 * entries 0, 2, 3 and the second's 0, 1, 3 (the stores after :2863 and
 * :2890), so each corner's pivot is the midpoint of its own point's pair.
 * Points 0 and n are the coincident head and tail pairs (:2747-2766), so
 * neither end's corners take a pivot.
 * @param depth The segment's scene depth.
 * @param xy The segment's four packed screen-XY entries.
 * @param second 0 for the first triangle, 1 for the second.
 * @param j The segment index, 1 to n.
 * @param n The record's segment count.
 * @return 1 when appended, 0 when the table is full. */
static int zap_replay_emit(float depth, const uint32_t xy[4], int second, int j, int n) OP_REQUIRES(sim) {
    if (s_pred_n >= ZAP_MAX_PRIMS) return 0;
    static const int ent[2][3] = { {0, 2, 3}, {0, 1, 3} };
    zap_pivot* pv = &s_pred_pv[s_pred_n];
    for (int k = 0; k < 3; k++) {
        const int e = ent[second][k];
        const int point = (e < 2) ? j - 1 : j;
        pv->ok[k] = (uint8_t)(point > 0 && point < n);
        pv->pt[k] = (uint8_t)point;
        const int pair = (e < 2) ? 0 : 2;
        zap_entry_mid(xy[pair], xy[pair + 1], &pv->x[k], &pv->y[k]);
        s_pred_xy[s_pred_n][k][0] = (int16_t)(xy[e] & 0xFFFFu);
        s_pred_xy[s_pred_n][k][1] = (int16_t)(xy[e] >> 16);
    }
    s_pred[s_pred_n++] = depth;
    return 1;
}

/** @brief Replays the routine's emit loop over one record's entry table, appending one depth per emitted triangle in emit order.
 *
 * The table holds two entries per chain point, {packed screen XY, SZ3 in the
 * upper half and the four-bit outcode in the lower}, and the routine reads
 * four consecutive entries per segment (:2839-2848). The record is dropped
 * whole when every projected entry shares an outcode bit (:2804-2806) or its
 * middle entry's SZ3 reaches ZAP.far_sz (:2821-2827); a segment is skipped on
 * its four corners' outcode AND, or when its ordering-table index, the SZ3
 * sum shifted down nine less two, is negative (:2843-2860), which is what
 * takes out a segment straddling the camera plane; one triangle of a segment
 * is skipped when its point's two entries coincide (:2863, :2890).
 * @param tab The record's projected entry table, 2n+2 entries.
 * @param n The record's segment count.
 * @return The triangles appended, or -1 when the prediction table filled. */
static int zap_replay(const uint32_t tab[][2], int n) OP_REQUIRES(sim) {
    const int first = s_pred_n;
    int off = 0xF;
    for (int e = 0; e <= 2 * n + 1; e++) off &= (int)(tab[e][1] & 0xFu);
    if (off != 0) return 0;
    const int mid_sz = (int)(int16_t)(uint16_t)(tab[n][1] >> 16);
    if (mid_sz - ZAP.far_sz >= 0) return 0;

    for (int j = 1; j <= n; j++) {
        uint32_t xy[4], w1[4];
        for (int k = 0; k < 4; k++) {
            xy[k] = tab[2 * j - 2 + k][0];
            w1[k] = tab[2 * j - 2 + k][1];
        }
        int oc = 0xF;
        for (int k = 0; k < 4; k++) oc &= (int)(w1[k] & 0xFu);
        if (oc != 0) continue;
        uint32_t sum = 0;
        for (int k = 0; k < 4; k++) sum += w1[k] >> 16;
        if ((int)(sum >> 9) - 2 < 0) continue;
        const float d = openpete_native_scene_viewz(
            openpete_worldvz_full((float)sum * 0.25f));
        if (xy[2] != xy[3] && !zap_replay_emit(d, xy, 0, j, n)) return -1;
        if (xy[0] != xy[1] && !zap_replay_emit(d, xy, 1, j, n)) return -1;
    }
    return s_pred_n - first;
}

/* One live record of the call: its replayed triangles in s_pred and where its
 * undisplaced middle chain point projects, the fallback for a primitive no
 * replayed triangle explains. */
typedef struct {
    uint32_t rec;
    int      n;
    int      tri0, tri_n;
    float    mid_depth;
    float    mx, my;
} zap_rec_t;
static zap_rec_t s_recs[ZAP_MAX_SLOTS] OP_GUARDED_BY(sim);
static int       s_recs_n OP_GUARDED_BY(sim);

/** @brief Projects a record's undisplaced middle chain point: head + (n / 2) * step, the routine's integer step.
 * @param c The call's inputs.
 * @param rec The record's address, covered by the snapshot.
 * @param n The segment count.
 * @param r Receives mid_depth, mx and my. */
static void zap_rec_mid(const zap_emu_ctx* c, uint32_t rec, int n, zap_rec_t* r) OP_REQUIRES_SHARED(sim) {
    int32_t P[3];
    for (int k = 0; k < 3; k++) {
        const int32_t h = (int32_t)zap_pre_word(rec + ZAP.head_off + 4u * (uint32_t)k);
        const int32_t t = (int32_t)zap_pre_word(rec + ZAP.head_off + 12u + 4u * (uint32_t)k);
        const int32_t ph = (k == 0) ? h - c->cam[0] : c->cam[k] - h;
        const int32_t pt = (k == 0) ? t - c->cam[0] : c->cam[k] - t;
        P[k] = ph + ((pt - ph) / n) * (n / 2);
    }
    uint32_t e[2] = { zap_pack(P[1], P[2]), (uint32_t)P[0] };
    zap_emu_rtps(c, e);
    r->mid_depth = openpete_native_scene_viewz(openpete_worldvz_full((float)(e[1] >> 16)));
    r->mx = (float)(int16_t)(e[0] & 0xFFFFu);
    r->my = (float)(int16_t)(e[0] >> 16);
}

/** @brief Gives an unexplained primitive the depth of the replayed triangle whose corners lie nearest its own, or with none the nearest live record's middle depth.
 * @param it The primitive, its depth rewritten. */
static void zap_pair_nearest(zap_item* it) OP_REQUIRES_SHARED(sim) {
    double best = INFINITY;
    for (int t = 0; t < s_pred_n; t++) {
        double d2 = 0.0;
        for (int k = 0; k < 3; k++) {
            const double dx = (double)it->p.x[k] - s_pred_xy[t][k][0];
            const double dy = (double)it->p.y[k] - s_pred_xy[t][k][1];
            d2 += dx * dx + dy * dy;
        }
        if (d2 < best) { best = d2; it->depth = s_pred[t]; }
    }
    if (s_pred_n > 0) return;
    const double cx = ((double)it->p.x[0] + it->p.x[1] + it->p.x[2]) / 3.0;
    const double cy = ((double)it->p.y[0] + it->p.y[1] + it->p.y[2]) / 3.0;
    for (int r = 0; r < s_recs_n; r++) {
        const double dx = cx - s_recs[r].mx, dy = cy - s_recs[r].my;
        if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; it->depth = s_recs[r].mid_depth; }
    }
}

/** @brief Reads the GTE state and the staged camera the routine loads for the call (:2564-2578).
 * @param cpu The guest CPU state.
 * @param v The tick's view, the matrix and the GTE offsets and H it read.
 * @param c Receives the inputs. */
static void zap_emu_ctx_read(CPUState* cpu, const ngte_view* v, zap_emu_ctx* c) {
    memcpy(c->M, v->cam_M, sizeof c->M);
    for (int k = 0; k < 3; k++) c->cam[k] = (int32_t)cpu->read_word(game_layout.cam.position + 4u * (uint32_t)k);
    c->ofx = v->ofx; c->ofy = v->ofy; c->H = (int)v->H_canon;
    c->state = (ZAP.rerand_hold >= 0) ? (int32_t)cpu->read_word(game_layout.game_state) : 0;
}

/* What the slot-order replay of one call leaves behind: how many live records
 * it could not replay, the last record that passed the box (the one the
 * rebase follows when it is the only one) and the last whose table was built
 * (the one the scratchpad still holds). */
typedef struct {
    int      unreplayed;
    uint32_t last_rec;
    int      last_n;
    uint32_t cal_rec;
    int      cal_n;
} zap_walk;

/** @brief Replays one live record: its undisplaced middle point, its entry table and its emit loop, appending its triangles to s_pred.
 *
 * Adds the record to s_recs and counts it in s_reached when its head passes
 * the box; a record whose count exceeds ZAP_MAX_SEG, whose inputs fall
 * outside the snapshot, or whose replay overflows the prediction table counts
 * as unreplayed.
 * @param cpu The guest CPU state.
 * @param c The call's inputs.
 * @param rec The record's address.
 * @param n Its segment count, non-zero.
 * @param w The call's walk, updated. */
static void zap_replay_record(CPUState* cpu, const zap_emu_ctx* c, uint32_t rec, int n,
                              zap_walk* w) OP_REQUIRES(sim) {
    if (n > ZAP_MAX_SEG || !zap_pre_covers(rec + ZAP.head_off, 24u)) { w->unreplayed++; return; }
    zap_rec_t* r = &s_recs[s_recs_n++];
    r->rec = rec; r->n = n; r->tri0 = s_pred_n; r->tri_n = 0;
    zap_rec_mid(c, rec, n, r);
    uint32_t tab[2 * ZAP_MAX_SEG + 2][2];
    const int st = zap_emu_table(cpu, c, rec, n, tab);
    if (st == 0) return;
    s_reached++;
    w->last_rec = rec; w->last_n = n;
    w->cal_rec = (st > 0) ? rec : 0u; w->cal_n = n;
    const int t = (st > 0) ? zap_replay(tab, n) : -1;
    if (t < 0) { w->unreplayed++; return; }
    r->tri_n = t;
}

/** @brief Replays every live record of the pool in slot order, the routine's own order, then calibrates against the scratchpad.
 *
 * Sets s_live and fills s_recs, s_pred and s_reached. The scratchpad holds
 * the last table the routine built, which is checked word for word against
 * that record's re-run.
 * @param cpu The guest CPU state.
 * @param c The call's inputs.
 * @param w Receives the walk. */
static void zap_replay_records(CPUState* cpu, const zap_emu_ctx* c, zap_walk* w) OP_REQUIRES(sim) {
    memset(w, 0, sizeof *w);
    for (uint32_t s = 0; s < ZAP.slots && s < ZAP_MAX_SLOTS; s++) {
        const uint32_t rec = ZAP.pool + s * ZAP.stride;
        if (!zap_pre_covers(rec + ZAP.packed_off, 4u)) break;
        const int n = (int)(zap_pre_word(rec + ZAP.packed_off) & 0xFFu);
        if (n == 0) continue;                          /* :2590 free slot */
        s_live++;
        zap_replay_record(cpu, c, rec, n, w);
    }
    uint32_t tab[2 * ZAP_MAX_SEG + 2][2];
    if (w->cal_rec && zap_emu_table(cpu, c, w->cal_rec, w->cal_n, tab) > 0)
        zap_emu_calibrate(cpu, tab, w->cal_n);
}

/** @brief Records the one projected record's head and tail, their tick projections and each chain point's tick half-width for the sub-tick rebase.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param rec The record's address.
 * @param n Its segment count. */
static void zap_rebase_capture(CPUState* cpu, const ngte_view* v, uint32_t rec, int n) {
    zap_rec_points(cpu, rec, s_head_w, s_tail_w);
    zap_project_point(v, s_head_w, NULL, &s_head_p);
    zap_project_point(v, s_tail_w, NULL, &s_tail_p);
    s_rec = rec;
    s_seg_n = n;
    static const double k_zero[3] = { 0.0, 0.0, 0.0 };
    for (int i = 0; i <= n; i++) s_hw_tick[i] = zap_half_width(v, k_zero, k_zero, i, n);
}

/** @brief Gives the captured primitives the replayed depths and pivots in emit order and sets s_accepted.
 *
 * Counts the records that contributed triangles in s_paired_recs and the
 * primitives whose corners differ from the replayed ones in s_corner_bad. */
static void zap_pair_emit_order(void) OP_REQUIRES(sim) {
    s_accepted = 1;
    for (int r = 0; r < s_recs_n; r++) if (s_recs[r].tri_n > 0) s_paired_recs++;
    for (int i = 0; i < s_item_n; i++) {
        s_item[i].depth = s_pred[i];
        s_item[i].paired = 1;
        s_item[i].pv = s_pred_pv[i];
        for (int k = 0; k < 3; k++)
            if (s_item[i].p.x[k] != (float)s_pred_xy[i][k][0] ||
                s_item[i].p.y[k] != (float)s_pred_xy[i][k][1]) { s_corner_bad++; break; }
    }
}

/** @brief Re-runs every live record in slot order and attaches each captured primitive to the segment that emitted it.
 *
 * When exactly one record reached the projection the sub-tick rebase follows
 * it. When every live record replayed and the replayed triangle count equals
 * the captured one the primitives pair in emit order; otherwise each takes
 * zap_pair_nearest's depth and counts in s_fallback.
 * @param cpu The guest CPU state.
 * @param v The tick's view. */
static void zap_pair_depths(CPUState* cpu, const ngte_view* v) OP_REQUIRES(sim) {
    s_pred_n = 0; s_rec = 0u; s_recs_n = 0; s_live = 0; s_reached = 0;
    zap_emu_ctx c;
    zap_emu_ctx_read(cpu, v, &c);
    zap_walk w;
    zap_replay_records(cpu, &c, &w);
    if (s_reached == 1) zap_rebase_capture(cpu, v, w.last_rec, w.last_n);
    if (!w.unreplayed && s_pred_n == s_item_n) { zap_pair_emit_order(); return; }
    for (int i = 0; i < s_item_n; i++) {
        zap_pair_nearest(&s_item[i]);
        s_fallback++;
    }
}

/* ---- feed ---------------------------------------------------------------- */

/** @brief Reports whether the zap feed is enabled, latched from OPENPETE_ZAP on first call.
 *
 * OPENPETE_ZAP=0 suppresses this feed alone. The guest draw still runs, so
 * an A/B prices the ribbons' painted pixels from one binary.
 * @return 0 when OPENPETE_ZAP starts with '0', else 1. */
static int zap_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_ZAP"); v = !(e && *e && *e == '0'); }
    return v;
}

/** @brief Computes where a corner sits along the ribbon, as its parameter on the tick's head-to-tail screen segment.
 *
 * The corner is projected onto the segment from s_head_p to s_tail_p and the
 * parameter clamped to 0..1. The chain the routine lays down runs from the
 * head to the tail by repeated addition of one step (:2638-2657), so this
 * parameter is that chain's own, exact at the two ends and linear between
 * them.
 * @param x The corner's screen x.
 * @param y The corner's screen y.
 * @return The parameter in 0..1; 0 when the two ends project within 1e-9
 *         squared pixels of each other, so the corner takes the head's
 *         displacement whole. */
static double zap_corner_pos(float x, float y) {
    const double ax = s_head_p.sx, ay = s_head_p.sy;
    const double dx = s_tail_p.sx - ax, dy = s_tail_p.sy - ay;
    const double n = dx * dx + dy * dy;
    if (n < 1e-9) return 0.0;
    const double t = (((double)x - ax) * dx + ((double)y - ay) * dy) / n;
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

/** @brief Scales one widened corner's offset from its chain point by that point's half-width scale.
 *
 * The chain point then moves with the corner through zap_rebase_xy. A corner
 * without a pivot, an inactive rebase, or a scale of 1 leaves it untouched.
 * @param x The corner's screen x, updated in place.
 * @param y The corner's screen y, updated in place.
 * @param pv The triangle's corner pivots.
 * @param k The corner index. */
static void zap_widen_xy(float* x, float* y, const zap_pivot* pv, int k) {
    if (!s_reb.active || !pv->ok[k]) return;
    const double scl = s_reb.hw_scale[pv->pt[k]];
    if (scl == 1.0) return;
    *x = (float)((double)pv->x[k] + ((double)*x - (double)pv->x[k]) * scl);
    *y = (float)((double)pv->y[k] + ((double)*y - (double)pv->y[k]) * scl);
}

/** @brief Moves one corner by the two ribbon ends' screen displacement blended at its own position along the ribbon.
 *
 * The rebase is skipped when inactive, so the canonical feed emits the
 * captured corner untouched; the position is reported either way.
 * @param x The corner's screen x, updated in place.
 * @param y The corner's screen y, updated in place.
 * @param pos Receives the corner's position along the ribbon in [0, 1], or NULL. */
static void zap_rebase_xy(float* x, float* y, double* pos) {
    const double t = zap_corner_pos(*x, *y);
    if (pos) *pos = t;
    if (!s_reb.active) return;
    *x = (float)((double)*x + s_reb.dhx + (s_reb.dtx - s_reb.dhx) * t);
    *y = (float)((double)*y + s_reb.dhy + (s_reb.dty - s_reb.dhy) * t);
}

/* The fed ribbon's half-width: the mean distance of every widened corner from
 * its own chain point, both as the feed placed them, measured across the
 * head-to-tail line the feed drew the ribbon along so that the ribbon's
 * stretch along its own length does not enter it. Accumulated per present for
 * OPENPETE_ZAP_TRACE. */
static double s_hw_sum;
static int    s_hw_n;

/** @brief Adds one widened corner's distance from its chain point to the fed half-width mean.
 *
 * The distance is measured perpendicular to the head-to-tail line the feed
 * drew, with the chain point rebased like the corner. A corner without a
 * pivot, or a ribbon of zero length, adds nothing.
 * @param cx The fed corner's screen x.
 * @param cy The fed corner's screen y.
 * @param pv The triangle's corner pivots.
 * @param k The corner index, 0 to 2. */
static void zap_width_note(float cx, float cy, const zap_pivot* pv, int k) {
    if (!pv->ok[k]) return;
    float mx = pv->x[k], my = pv->y[k];
    zap_rebase_xy(&mx, &my, NULL);
    const double ux = (s_tail_p.sx + s_reb.dtx) - (s_head_p.sx + s_reb.dhx);
    const double uy = (s_tail_p.sy + s_reb.dty) - (s_head_p.sy + s_reb.dhy);
    const double len = hypot(ux, uy);
    if (!(len > 0.0)) return;
    s_hw_sum += fabs(((double)cx - (double)mx) * uy - ((double)cy - (double)my) * ux) / len;
    s_hw_n++;
}

/** @brief Feeds one held ribbon primitive to the native scene as one or two world triangles.
 *
 * The corners are widened and rebased first. The depth is the held pairing
 * depth, or with the rebase active the view-Z blended between the head and
 * tail at the mean position of the corners. A quad splits into corners
 * {0,1,2} and {1,3,2}; a triangle feeds once. A primitive that is neither
 * is skipped.
 * @param h The held primitive with its depth and pivots. */
static void zap_add_prim(const zap_held* h) OP_REQUIRES(sim) {
    const ngte_prim* p = &h->p;
    float depth = h->depth;
    if (p->nv != 3 && p->nv != 4) return;
    /* Rebase the corners first: the depth the triangle takes is the view-Z of
     * its own mean position along the ribbon, which is the same quantity the
     * routine keys its ordering-table bucket on (:2849-2860, the four corners'
     * mean SZ3). */
    float px[4], py[4];
    double pos_sum = 0.0;
    for (int k = 0; k < p->nv; k++) {
        px[k] = p->x[k]; py[k] = p->y[k];
        double t = 0.0;
        if (k < 3) zap_widen_xy(&px[k], &py[k], &h->pv, k);
        zap_rebase_xy(&px[k], &py[k], &t);
        pos_sum += t;
        if (k < 3) zap_width_note(px[k], py[k], &h->pv, k);
    }
    if (s_reb.active) {
        const double tm = pos_sum / (double)p->nv;
        const double vz = s_reb.head_m3 + (s_reb.tail_m3 - s_reb.head_m3) * tm;
        depth = openpete_native_scene_viewz(openpete_worldvz_full((float)vz));
    }
    ngte_tpage tp;
    const int blend = ngte_prim_tpage_blend(p, &tp);
    const int last = p->nv - 1;           /* a triangle fills the unread corner 3 */
    const int idx[4] = { 0, 1, 2, last };
    float x[4], y[4], u[4], v[4];
    uint32_t rgb[4];
    for (int k = 0; k < 4; k++) {
        const int j = idx[k];
        x[k] = px[j]; y[k] = py[j];
        u[k] = (float)p->u[j]; v[k] = (float)p->v[j];
        rgb[k] = p->rgb[j];
    }
    s_fed_tris += (uint32_t)ngte_feed_flat_quad(x, y, u, v, rgb, depth,
                                                p->textured ? &tp : NULL, blend, p->nv);
}

/** @brief Feeds a held set of ribbon primitives to the native scene under the tracer tag.
 *
 * A no-op when OPENPETE_ZAP is 0 or n is not positive. Opens the frame's
 * scene and resets the fed half-width accumulators first.
 * @param set The held primitives.
 * @param n The count of primitives. */
static void zap_feed_set(const zap_held* set, int n) OP_REQUIRES(sim) {
    if (!zap_feed_enabled()) return;
    if (n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_TRACER(OPENPETE_TAG_TRACER_ZAP, 0));
    s_hw_sum = 0.0; s_hw_n = 0;
    for (int i = 0; i < n; i++) zap_add_prim(&set[i]);
    openpete_native_scene_set_tag(0);
}

/** @brief Prints one OPENPETE_ZAP_TRACE line for a present that fed this producer.
 *
 * Enabled by OPENPETE_ZAP_TRACE set to a value other than 0, latched on first
 * call. The line carries the live record count, the accepted record, the
 * screen points the feed placed the ribbon's two ends at and the mean fed
 * width. On a canonical present those points are the routine's own
 * projections of the record's head and tail; on a sub-tick present they are
 * those two points moved by the rebase. A present whose live count is zero
 * fed nothing and measured nothing.
 * @param leg The present's leg name ("canon" or "inter").
 * @param alpha The present's sub-tick alpha. */
static void zap_trace(const char* leg, double alpha) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_ZAP_TRACE"); on = (e && *e && *e != '0'); }
    if (!on) return;
    fprintf(stderr, "[zap-screen vbl=%u leg=%s alpha=%.3f] live=%d rec=%08X "
                    "head=(%.2f,%.2f) tail=(%.2f,%.2f) width=%.3f wn=%d\n",
            openpete_feed_key_now(), leg, alpha, s_live, s_rec,
            s_head_p.sx + s_reb.dhx, s_head_p.sy + s_reb.dhy,
            s_tail_p.sx + s_reb.dtx, s_tail_p.sy + s_reb.dty,
            s_hw_n ? 2.0 * s_hw_sum / (double)s_hw_n : 0.0, s_hw_n);
}

/** @brief Appends the call's captured primitives to the held set the sub-tick replay re-feeds.
 *
 * The held set holds at most ZAP_MAX_PRIMS primitives; on_zap_draw empties it
 * on the canonical epoch's first call. */
static void zap_hold(void) {
    for (int i = 0; i < s_item_n && s_tick_n < ZAP_MAX_PRIMS; i++) {
        s_tick[s_tick_n].p = s_item[i].p;
        s_tick[s_tick_n].depth = s_item[i].depth;
        s_tick[s_tick_n].pv = s_item[i].pv;
        s_tick_n++;
    }
}

/* The census totals over every call it printed; each call prints the running
 * totals, so the last line is the run's. */
static struct {
    unsigned long calls, multi, live, reached, paired_recs, prims, fallback, depth1,
                  corner_bad, cal_words, cal_bad;
} s_ztot OP_GUARDED_BY(sim);   /* thread-roles: census on sim */

/** @brief Prints the per-call zap census for VBLs inside the OPENPETE_ZAP_CENSUS range.
 *
 * OPENPETE_ZAP_CENSUS=LO-HI, or a single VBL. One line per call: the
 * polybuf interval, the primitives captured by GPU code, the live records,
 * the records that reached the projection, whether the replay was accepted,
 * the records it paired, the triangles it predicted, the native triangles
 * fed since fed_before, the primitives left unpaired, those given a nearest
 * depth, those still at depth 1, the paired primitives whose corners differ
 * from the replay, and the scratchpad words compared and differing. Then one
 * line per primitive with its three corners, colour, first UV, page and
 * depth, and the running totals. A capture whose fed count is zero
 * measured nothing.
 * @param start The polybuf cursor before the call.
 * @param end The polybuf cursor after the call.
 * @param fed_before s_fed_tris before the call. */
static void zap_census(uint32_t start, uint32_t end, uint32_t fed_before) OP_REQUIRES(sim) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_ZAP_CENSUS", openpete_frame)) return;
    int unpaired = 0, depth1 = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (!s_item[i].paired) unpaired++;
        if (s_item[i].depth == 1.0f) depth1++;
    }
    s_ztot.calls++; s_ztot.multi += (s_reached > 1); s_ztot.live += (unsigned long)s_live;
    s_ztot.reached += (unsigned long)s_reached; s_ztot.paired_recs += (unsigned long)s_paired_recs;
    s_ztot.prims += (unsigned long)s_item_n; s_ztot.fallback += (unsigned long)s_fallback;
    s_ztot.depth1 += (unsigned long)depth1; s_ztot.corner_bad += (unsigned long)s_corner_bad;
    s_ztot.cal_words += (unsigned long)s_cal_words; s_ztot.cal_bad += (unsigned long)s_cal_bad;
    fprintf(stderr, "[zap-census f=%u] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, start, end, end - start, s_item_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_item_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    fprintf(stderr, " live=%d reached=%d accepted=%d paired_recs=%d predicted=%d fed_tris=%u"
                    " unpaired=%d fallback=%d depth1=%d corner_bad=%d cal=%d/%d\n",
            s_live, s_reached, s_accepted, s_paired_recs, s_pred_n, s_fed_tris - fed_before,
            unpaired, s_fallback, depth1, s_corner_bad, s_cal_bad, s_cal_words);
    fprintf(stderr, "[zap-census total f=%u] calls=%lu multi=%lu live=%lu reached=%lu paired_recs=%lu "
                    "prims=%lu fallback=%lu depth1=%lu corner_bad=%lu cal_words=%lu cal_bad=%lu\n",
            openpete_frame, s_ztot.calls, s_ztot.multi, s_ztot.live, s_ztot.reached,
            s_ztot.paired_recs, s_ztot.prims, s_ztot.fallback, s_ztot.depth1, s_ztot.corner_bad,
            s_ztot.cal_words, s_ztot.cal_bad);
    /* Each triangle's first corner and the scene depth the pairing gave it:
     * the two quantities the feed places it by. */
    for (int i = 0; i < s_item_n; i++)
        fprintf(stderr, "[zap-tri f=%u] %d a=(%.0f,%.0f) b=(%.0f,%.0f) c=(%.0f,%.0f)"
                        " rgb=%06X uv=(%d,%d) tp=%d depth=%.6f\n",
                openpete_frame, i, s_item[i].p.x[0], s_item[i].p.y[0],
                s_item[i].p.x[1], s_item[i].p.y[1], s_item[i].p.x[2], s_item[i].p.y[2],
                s_item[i].p.rgb[0] & 0xFFFFFFu, s_item[i].p.u[0], s_item[i].p.v[0],
                s_item[i].p.tpage, s_item[i].depth);
}

/* ---- hook ---------------------------------------------------------------- */

static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_TRACER, .prof_tag = "zap.srend", .st = &s_hb,
};

/** @brief Hooks the zap routine: runs it once through the shared bracket, then captures, pairs, holds and feeds its primitives.
 *
 * The world draw's call is the only one in either image, so the hook takes
 * every entry (Spyro 2 draw/func_800155A0.s:34, Spyro 3
 * draw/func_8001E460.s:32). The polybuf interval the call allocated is
 * decoded, paired to the replayed segment depths, and held for the tick's
 * sub-tick replay; with the native scene live and RP_TRACER enabled the held
 * set is fed unrebased. The census runs on every bracketed call.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the bracket declines (the ordinary dispatch runs the
 *         routine), else hook_bracket_end's result. */
static int on_zap_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    uint32_t startp, endp; int feed, verify;
    zap_snapshot(cpu);
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    s_item_n = 0; s_pred_n = 0; s_live = 0; s_reached = 0; s_accepted = 0;
    s_paired_recs = 0; s_fallback = 0; s_corner_bad = 0; s_cal_words = 0; s_cal_bad = 0;
    memset(&s_reb, 0, sizeof s_reb);   /* the canonical feed emits the captured corners */
    const uint32_t fed_before = s_fed_tris;
    /* The epoch's first call retires the previous epoch's set (cut_held_sets_capture). */
    if (cut_held_sets_capture(&s_sets)) s_tick_n = 0;
    if (endp > startp) {
        zap_capture_prims(cpu, startp, endp);
        ngte_view view;
        ngte_view_build(cpu, &view);
        zap_pair_depths(cpu, &view);
        zap_hold();
        if (feed && render_path_enabled(RP_TRACER)) {
            render_material_level_push(cpu);
            zap_feed_set(s_tick, s_tick_n);
            zap_trace("canon", 1.0);
        }
    }
    zap_census(startp, endp, fed_before);
    return hook_bracket_end(&k_bracket);
}

/* ---- sub-tick rebase ------------------------------------------------------
 *
 * The triangles are the guest's own int16 output, built under the tick's
 * camera from the tick's record, so a sub-tick present that re-feeds them
 * draws the ribbon where the tick drew it: neither the camera's motion nor the
 * record's own reaches it. The rebase measures each end's screen displacement
 * as the difference of two projections - the world point the present shows
 * under the staged camera, against the point the tick drew under the tick's
 * camera - and moves every corner by the pair blended at its own position
 * along the chain. At the end of the tick's interval the displaced points and
 * the staged camera are the tick's, so both projections take identical inputs
 * through identical arithmetic and every term is exactly zero; the replay then
 * emits the captured corners verbatim instead, which makes that exactness
 * structural rather than a matter of float agreement. */

/* A beam end's speed ceiling, in full world units per tick. The table is keyed
 * by slot address, so a slot a new record respawns into inside one tick pairs
 * across two different beams and would lerp as a sweep between them. The
 * segment-count test below catches a reuse that changed the chain's length;
 * this catches the rest, at a distance no end covers in a tick while staying
 * inside the +/-0x5400 box the routine admits it from. */
#define ZAP_REUSE_CEILING 8192.0

/** @brief Computes the record's head and tail displacement between the pose the tick drew and the pose this present shows.
 *
 * The displacement is (previous - current) * (1 - alpha) in full world units,
 * read from the interpolation snapshot pair keyed by the record's address.
 * With no usable previous record (a beam that spawned this tick, a pair
 * whose prev was not observed up to curr (cut_feeder_pair_is_tick), a changed
 * segment count, or an end that moved more than ZAP_REUSE_CEILING) both
 * offsets are zero, and the
 * ribbon holds its tick pose in the world with only the camera's motion
 * rebasing it.
 * @param alpha The present's sub-tick alpha.
 * @param off_head Receives the head's displacement.
 * @param off_tail Receives the tail's displacement.
 * @return 1 when the displacement came from a usable pair, else 0. */
static int zap_record_pair(double alpha, double off_head[3], double off_tail[3]) {
    for (int k = 0; k < 3; k++) off_head[k] = off_tail[k] = 0.0;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(s_rec);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    if ((uint32_t)o->blob_len < ZAP.head_off + 24u ||
        (uint32_t)o->blob_len < ZAP.packed_off + 4u) return 0;
    if (!cut_feeder_pair_is_tick("zap", s_rec, o, ZAP.head_off, 24u)) return 0;
    uint32_t pp, cp;
    memcpy(&pp, o->prev + ZAP.packed_off, 4);
    memcpy(&cp, o->curr + ZAP.packed_off, 4);
    if ((pp & 0xFFu) != (cp & 0xFFu)) return 0;      /* the chain changed length */
    int32_t ph[3], pt[3];
    memcpy(ph, o->prev + ZAP.head_off, 12);
    memcpy(pt, o->prev + ZAP.head_off + 12u, 12);
    const double rest = 1.0 - alpha;
    double sq_h = 0.0, sq_t = 0.0;
    for (int k = 0; k < 3; k++) {
        const double dh = (double)ph[k] - (double)(int32_t)s_head_w[k];
        const double dt = (double)pt[k] - (double)(int32_t)s_tail_w[k];
        sq_h += dh * dh; sq_t += dt * dt;
        off_head[k] = dh * rest;
        off_tail[k] = dt * rest;
    }
    if (sq_h > ZAP_REUSE_CEILING * ZAP_REUSE_CEILING ||
        sq_t > ZAP_REUSE_CEILING * ZAP_REUSE_CEILING) {
        for (int k = 0; k < 3; k++) off_head[k] = off_tail[k] = 0.0;
        return 0;
    }
    return 1;
}

/** @brief Rebases the held set onto the pose this present shows.
 *
 * The staged camera resolves through the sub-tick view, so building a view
 * here reads the interpolated transform the rest of the pass draws under.
 * The rebase stays inactive with no accepted record, an empty held set, or
 * alpha at or above 1.
 * @param cpu The guest CPU state.
 * @param alpha The present's sub-tick alpha. */
static void zap_rebase_all(CPUState* cpu, double alpha) {
    memset(&s_reb, 0, sizeof s_reb);
    if (!s_rec || s_tick_n <= 0) return;
    /* At the end of the tick's own interval the replay must reproduce the
     * canonical present exactly, so it emits the captured corners and the
     * paired depths verbatim rather than a re-projection that agrees with them
     * only to the last bit. */
    if (alpha >= 1.0) return;
    double off_head[3], off_tail[3];
    zap_record_pair(alpha, off_head, off_tail);
    ngte_view v;
    ngte_view_build(cpu, &v);
    ngte_proj h, t;
    zap_project_point(&v, s_head_w, off_head, &h);
    zap_project_point(&v, s_tail_w, off_tail, &t);
    s_reb.dhx = h.sx - s_head_p.sx;
    s_reb.dhy = h.sy - s_head_p.sy;
    s_reb.dtx = t.sx - s_tail_p.sx;
    s_reb.dty = t.sy - s_tail_p.sy;
    s_reb.head_m3 = h.m3;
    s_reb.tail_m3 = t.m3;
    zap_width_scales(&v, off_head, off_tail);
    s_reb.active = 1;
}

/* Sub-tick re-extract. The primitives are the guest's own output and are gone
 * once the polybuf is reused, so the replay re-feeds the set the canonical tick
 * captured, rebased onto the pose this present shows: the record's head and
 * tail carry the ribbon's two ends, every corner between them moves by the
 * pair blended at its own position along the chain, and each widened corner's
 * offset from its chain point scales by the change in half-width. The jitter is not
 * re-derived - the routine draws and stores a fresh random halfword per point
 * inside its own call (:2673-2685), and this replay re-feeds the tick's
 * primitives, so a sub-tick present shows the tick's jitter rather than a new
 * one. The rebase runs only inside the tick's interval; at its end the replay
 * emits the captured corners and the paired depths verbatim and so reproduces
 * the canonical present exactly, which is what the spine's identity leg
 * measures. */
void zap_reextract(CPUState* cpu, int bracketed, double alpha, int hold) OP_REQUIRES(sim) {
    const int pick = cut_held_sets_enter(&s_sets, hold);
    if (hook_reextract_ready_held(RP_TRACER, pick)) {
        /* On a held cut the staged camera is tick N-1's, the camera the shown
         * set was drawn under, and its record state is not the one s_rec
         * paired: the captured corners feed verbatim. */
        if (pick >= 0) memset(&s_reb, 0, sizeof s_reb);
        else if (bracketed) zap_rebase_all(cpu, alpha);
        render_material_level_push(cpu);
        zap_feed_set(s_tick, s_tick_n);
        zap_trace("inter", bracketed ? alpha : 1.0);
    }
    cut_held_sets_leave(&s_sets, pick);
}

/* The hook address is the layout's zap draw site, bound at boot: a
 * static initializer cannot read another TU's const. A game with no located
 * routine leaves it 0 and the registry dispatches nothing. */
static feature_dispatch_entry_t zap_dispatch[] = {
    { 0u, on_zap_draw, 0u },
    { 0u, NULL, 0u },
};

/// @brief Binds the module's dispatch entry to the layout's zap draw address.
static void zap_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    zap_dispatch[0].addr = game_layout.zap.hook;
}

const feature_module_t zap_module = {
    .name           = "zap_native",
    .apply_options  = zap_apply_options,
    .dispatch_table = zap_dispatch,
};
