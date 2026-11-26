/* games/spyro-1/renderer/reextract.c - Spyro 1's sub-tick re-feed orchestrator.
 * See reextract.h for the contract. */

#include <stdatomic.h>

#include "reextract.h"
#include "openpete_camlock.h"       /* render camera lock (G) - bracket override */
#include "openpete_interp.h"
#include "openpete_profile.h"          /* the re-extract's per-producer regions */
#include "renderer/moby_serial.h"   /* moby_pair_reborn, the allocation census */
#include "renderer/interp_lerp.h"    /* lerp16/32, lerp_matrix, lerp_vec3 */
#include "renderer/interp_regions_gen.h"
#include "renderer/reextract_view.h"
#include "openpete_pace_log.h"        /* snapshot ring query + op_now/OP_NS_PER_SEC */
#include "openpete_interp_objs.h"   /* per-object (moby) prev/curr table */
#include "openpete_native_render.h" /* register callback + scene API */
#include "openpete.h"                 /* openpete_runahead_get */
#include "openpete_psx.h"
#include "openpete_subtick_view.h"           /* openpete_psx_to_host_span */
#include "psx_runtime_internal.h"            /* CPUState (read_word) */
#include "renderer/render_paths.h"  /* canon-drawn mask (load-window census) */
#include "interp_regions.h"   /* game_interp_moby_key */
#include "renderer/cut_guard.h"        /* the shared camera-cut guard and its census */
#include "renderer/shadow.h"           /* shadow_replay_prev_list */
#include "renderer/flame.h"          /* the shared fire-breath producer + its bracket */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int openpete_ra_input_edge; /* runahead: led branch changed this tick */

/** @brief Reports whether the runahead correction rebase is enabled (default on; OPENPETE_RA_REBASE=0 disables).
 *
 * On an input-edge tick the led pair jumps prediction branches, and a lerp
 * from the old branch's prev to the new branch's curr moves the object
 * backward. The rebase takes prev from the last shown bytes, so the
 * present moves from the shown position to the corrected branch over one
 * tick. It applies only in the led-camera configuration
 * (OPENPETE_RA_CAM_COMMIT=0), in both the camera and the Spyro arm: with
 * the default committed-timeline camera the camera pair never crosses a
 * branch, and under analog input the stick changes almost every tick, so
 * the edge fires continuously and the re-anchor bunches Spyro's motion
 * into the late presents and steps backward at negative alpha. Measured in
 * the led-camera configuration on a release build: -36 and -88 units per
 * present unrebased at N=1 and N=2, -20 and -70 rebased. The variable is
 * read once and cached.
 * @return 1 when enabled, else 0. */
static int ra_rebase_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_RA_REBASE"); v = !(e && *e == '0'); }
    return v;
}
/* shown-state buffers live below the region defines (CAM_BYTES) */
#include "spyro-1/gen/spyro1_symbols.gen.h"
#include "game_layout.h"
#include "renderer/player.h"   /* per-call records of the player hooks */
#include "renderer/player_game.h"   /* PLAYER_PARTS_MAX */
#include "openpete_thread_roles.h"

/* The engine layout of the player feeder: every span the record loop stages
 * and every scratch block it overlays. */
#define PLAYER_L (game_layout.player)

extern unsigned openpete_frame;

#define CAM_BYTES     0x110u
#define G_HUD_COUNT   12u
#define MOBY_STRIDE   0x58u
#define MOBY_STATE    0x48u
#define MOBY_POS      0x0Cu         /* Vector3D x,y,z (int32) */
/** @brief Resolves a ring region index by name from the registered table, caching the lookup.
 *
 * Indices are looked up by name (interp_regions_gen.h) because a restated
 * index selects the wrong region's bytes once a row is inserted. Each
 * REGION_ACCESSOR function holds its own cache, initialised to -2.
 * @param name The region name, a registered row such as "g_Camera".
 * @param cache The caller's cache; -2 means not yet looked up.
 * @return The row index, -1 when no such region is registered. */
static int region_of(const char* name, _Atomic int* cache) {
    int v = atomic_load_explicit(cache, memory_order_acquire);
    if (v != -2) return v;
    const int want = game_interp_row_index(name);
    atomic_compare_exchange_strong(cache, &v, want);
    return want;
}
#define REGION_ACCESSOR(fn, nm) \
    static int fn(void) { static _Atomic int c = -2; return region_of(nm, &c); }
REGION_ACCESSOR(CAM_REGION,    "g_Camera")
REGION_ACCESSOR(SPYRO_REGION,  "g_Spyro")
REGION_ACCESSOR(FLAME_REGION,  "g_SpyroFlame")
REGION_ACCESSOR(SHADOW_REGION, "g_SpyroShadow")
#define SPYRO_SAVE    0x28u         /* g_Spyro bytes bracketed: pos(0x00) + per-seg anim(..0x26) */

/* Runahead rebase shown-state stores (see ra_rebase_on above). */
static uint8_t  s_shown_cam[CAM_BYTES];
static int      s_shown_cam_ok = 0;
static uint8_t  s_shown_sp[12];          /* Spyro m_Position as last shown */
static int      s_shown_sp_ok = 0;
#define FLAME_SAVE    0xD0u         /* flame bytes bracketed (covers pos + attach @ 0xB8..0xCC) */
#define SHADOW_SAVE   0x28u         /* the whole D_SPYROSHADOW region (game_layout.player.shadow_len):
                                     * the shadow record loop restages it */
#define SHADOW_POS_OFF 0x10u        /* m_Position (ground anchor, Vector3D int32) */

/* Per-moby RAM bracket save slots - restored after re-extract so the next sim
 * tick reads canonical RAM. Sized generously for any level's live moby count. */
#define MOBY_SAVE_MAX 1024
/* `staged` records that this moby's sub-tick writes went to the view's copy
 * rather than to guest RAM, so it needs no restore. `bytes` is captured
 * either way: for a staged record it lets the integrity check below PROVE
 * guest RAM was left alone, which is the claim staging is making. */
typedef struct { uint32_t addr; uint8_t staged; uint8_t bytes[MOBY_STRIDE]; } moby_save_t;

/* g_Particles pool (smoke / triangles / line particles): stride-0x20 array behind
 * a pointer; pos = X@lo16(+0x04), Y@hi16(+0x04), Z@lo16(+0x08) (int16, world/4).
 * The spine lerps each active particle's position by its per-object alpha (keyed
 * by slot address in the interp-objs table) so they animate at the present rate.
 * Only +0x04/+0x08 are bracketed (saved/restored). */
#define PART_STRIDE     0x20u
#define PART_HDR_SEL    8u           /* (hdr>>8)&0xFF: 0xFF=end, &0x80=inactive */
#define PART_POS_OFF    0x04u        /* X(lo16)/Y(hi16) word; Z(lo16) at +0x08 */
#define PART_SAVE_MAX   512
/* `staged` mirrors the moby table: writes went to the view's copy, so no
 * restore is owed. w1/w2 are captured either way so the integrity check can
 * prove guest RAM was left alone. */
typedef struct { uint32_t addr, w1, w2; uint8_t staged; } part_save_t;  /* +0x04, +0x08 */

#define PART_SEL_HIDDEN 0xFEu        /* inactive, not the end of the pool */

/** @brief The g_Particles slot at `pa` as the canonical present showed it, under --runahead.
 *
 * The led tick's bytes the objs table captured at the speculation head are
 * staged over the committed guest bytes when they differ. Guest memory is never
 * written: when the view refuses staging the slot keeps its committed bytes.
 * @param pa the slot's guest address
 * @param gh the slot's guest host pointer
 * @param o  the slot's interp-objs entry
 * @return the host pointer to read the shown slot from */
static uint8_t* particle_stage_shown(uint32_t pa, uint8_t* gh, const openpete_interp_obj_t* o) {
    uint32_t n = o->blob_len < PART_STRIDE ? o->blob_len : PART_STRIDE;
    if (memcmp(gh, o->curr, n) == 0) return gh;
    uint8_t* st = (uint8_t*)openpete_subtick_view_stage(pa, PART_STRIDE);
    if (!st) return gh;
    memcpy(st, o->curr, n);
    return st;
}

/** @brief Stage the selector byte of the g_Particles slot at `pa`, leaving guest memory unwritten.
 *
 * PART_SEL_HIDDEN skips the slot and 0xFF ends the pool walk; a refused stage
 * keeps the committed byte. */
static void particle_stage_sel(uint32_t pa, uint8_t sel) {
    uint8_t* st = (uint8_t*)openpete_subtick_view_stage(pa, PART_STRIDE);
    if (st) st[PART_HDR_SEL / 8u] = sel;
}

/** @brief One past the highest g_Particles slot the objs table captured.
 *
 * This is the led pool's extent under --runahead. The capture walk records only
 * active slots, and the led pool can reach past the committed pool's end
 * sentinel. */
static uint32_t particle_led_extent(uint32_t ppool) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 320u; i++)
        if (openpete_interp_objs_find(ppool + i * PART_STRIDE)) n = i + 1u;
    return n;
}

/** @brief Stage every slot of a fixed record pool as the led capture holds it, under --runahead.
 *
 * The pool is `n` slots of `stride` bytes at `base`. A captured slot shows its
 * captured bytes, and a slot live only in the committed guest RAM has the
 * liveness bits `hide_mask` of its word at `hide_off` cleared so its feeder
 * skips it. Guest memory is never written; a refused stage keeps the committed
 * bytes. */
static void stage_led_fixed_pool(uint32_t base, uint32_t n, uint32_t stride,
                                 uint32_t hide_off, uint32_t hide_mask) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = base + i * stride;
        const uint8_t* gh = (const uint8_t*)openpete_psx_to_host_span(a, stride);
        if (!gh) break;
        const openpete_interp_obj_t* o = openpete_interp_objs_find(a);
        if (o) {
            uint32_t len = o->blob_len < stride ? o->blob_len : stride;
            if (memcmp(gh, o->curr, len) == 0) continue;
            uint8_t* st = (uint8_t*)openpete_subtick_view_stage(a, stride);
            if (st) memcpy(st, o->curr, len);
            continue;
        }
        uint32_t w; memcpy(&w, gh + hide_off, 4);
        if (!(w & hide_mask)) continue;
        uint8_t* st = (uint8_t*)openpete_subtick_view_stage(a, stride);
        if (st) { w &= ~hide_mask; memcpy(st + hide_off, &w, 4); }
    }
}

/* g_Camera field offsets (verified in camera.h / the feeders):
 *   +0x00 m_ProjectionMatrix (SHORTMATRIX: short[3][3]+pad)
 *   +0x14 m_ViewMatrix       (SHORTMATRIX)
 *   +0x28 m_Position         (Vector3D: int32 x,y,z) */
#define CAM_OFF_PROJ  0x00u
#define CAM_OFF_VIEW  0x14u
#define CAM_OFF_POS   0x28u

/* One wall-time sim tick (--tick-scale aware; run.c rescales the global),
 * passed to the ring query for the prev/curr pairing + dedupe verdict; the
 * alpha is recomputed below from the snapshots' measured timestamps, with the
 * denominator capped at one tick (interp_period_ns) so a stale prev after a
 * static stretch can't stretch a resume. */
#define TICK_PERIOD_NS  g_openpete_tick_period_ns

/** @brief Returns the interpolation denominator for a prev/curr pair, capped at one tick period.
 *
 * The ring rolls only on a change, so after an object or the camera holds
 * its bytes for K ticks and then moves, curr_ns - prev_ns spans K ticks;
 * dividing by it moves the object at 1/K speed for one interval and jumps
 * the remaining (1 - 1/K) at the next roll. Interpolation spans one tick
 * (one tick of lag, no extrapolation), so the cap leaves a steady object
 * (gap of one tick) unchanged and sweeps a resume's full delta over one
 * tick. The cap is g_openpete_tick_period_ns, which follows --tick-scale.
 * A non-positive span returns one tick period: under deterministic
 * capture a held state's duplicate rolls can advance past the grid point
 * the next distinct state lands on. OPENPETE_INTERP_PERIOD_UNCAP=1
 * returns the uncapped span, read once and cached.
 * @param prev_ns The prev snapshot's timestamp, nanoseconds.
 * @param curr_ns The curr snapshot's timestamp, nanoseconds.
 * @return The denominator, nanoseconds. */
static double interp_period_ns(op_time_t prev_ns, op_time_t curr_ns) {
    op_time_t p = curr_ns - prev_ns;
    /* Non-positive span: the pair still brackets one tick, so price it at the
     * nominal one. Under deterministic capture the stamps are grid points
     * counting distinct states, and a held state's duplicate rolls advance
     * past the grid point the next distinct state lands on - that one pair
     * measures a span at or below zero, and dividing by it turns the sub-tick
     * phase into a backward extrapolation of a forward motion. */
    if (p <= 0) return (double)g_openpete_tick_period_ns;
    /* OPENPETE_INTERP_PERIOD_UNCAP=1: diagnostic only; returns the uncapped
     * span, so the sweep covers the full distinct-roll interval. */
    static int s_uncap = -1;
    if (s_uncap < 0) { const char* e = getenv("OPENPETE_INTERP_PERIOD_UNCAP"); s_uncap = (e && *e && *e != '0'); }
    if (s_uncap) return (double)p;
    /* Cap at one wall-time tick (g_openpete_tick_period_ns, --tick-scale
     * aware), so a sweep spans one scaled tick. */
    return (double)(p > g_openpete_tick_period_ns ? g_openpete_tick_period_ns : p);
}

/** @brief Reports whether the predicted-tangent Hermite interpolation (pred1) is enabled.
 *
 * On by default; OPENPETE_INTERP_LINEAR set to a value not starting with
 * '0' selects the linear lerp. Read once and cached.
 * @return 1 when pred1 is enabled, 0 for linear. */
static int hermite_enabled(void) {
    static int e = -1;
    if (e < 0) { const char* s = getenv("OPENPETE_INTERP_LINEAR"); e = (s && *s && *s != '0') ? 0 : 1; }
    return e;
}

/* ---- Bracket-integrity self-check --------------------------------------------
 * The sub-tick present below publishes lerped values for the gameplay-readable
 * spans (g_Camera / g_Spyro / flame / shadow, plus moby & particle pool slots)
 * through the sub-tick view; a span the view refuses to stage is written into
 * PSX RAM instead and its canonical bytes restored before the game fiber
 * resumes. That restore discipline is exercised ONLY on sub-tick presents -
 * which the headless oracle never runs - so a missed or mis-guarded restore
 * would silently perturb gameplay state and surface only as an oracle drift
 * later. With
 * OPENPETE_BRACKET_CHECK set, hash the fixed DIRECT regions before any write and
 * re-check them after all restores; a mismatch means this present leaked into
 * gameplay RAM, logged on the same frame. Read-only, debug-gated, inert when
 * off. (Moby/particle pools are dynamic, so they're checked against their save
 * tables at the end of the present, next to their restore loops.) */
/** @brief Reports whether OPENPETE_BRACKET_CHECK enables the bracket-integrity self-check.
 *
 * Read once and cached.
 * @return 1 when enabled, else 0. */
static int bracket_check_enabled(void) {
    static int e = -1;
    if (e < 0) { const char* s = getenv("OPENPETE_BRACKET_CHECK"); e = (s && *s && *s != '0'); }
    return e;
}
/** @brief FNV-1a/64 of len guest bytes at psx_addr; 0 when the span does not map. */
static uint64_t bc_hash(uint32_t psx_addr, uint32_t len) {
    const uint8_t* h = (const uint8_t*)openpete_psx_to_host_span(psx_addr, len);
    uint64_t x = 1469598103934665603ULL;   /* FNV-1a/64 */
    if (!h) return 0;
    for (uint32_t i = 0; i < len; ++i) { x ^= h[i]; x *= 1099511628211ULL; }
    return x;
}

/** @brief Interpolates one coordinate between prev and curr with the pred1 cubic Hermite, clamped to the segment.
 *
 * A linear lerp holds velocity constant within each tick and changes it
 * at every tick boundary by the second difference of the sampled path;
 * that change measured a median of 5.5% of camera speed and above 100% on
 * sharp turns, at any display rate. pred1 uses the centred start tangent
 * m0 = 0.5 (curr - prev2) and the backward end tangent m1 = curr - prev:
 * the same one-tick latency as linear, with about half the boundary
 * velocity change. The result is clamped to [min(prev, curr),
 * max(prev, curr)]: at a path-polyline corner the centred tangent
 * overshoots the endpoint and returns, and the clamp is exact for
 * constant-velocity segments, where the cubic equals the lerp. Callers
 * fall back to the lerp when prev2 is absent or across a cut.
 * @param prev2 The sample one tick before prev.
 * @param prev The segment start.
 * @param curr The segment end.
 * @param t The sub-tick phase.
 * @return The interpolated value. */
static double hermite1(double prev2, double prev, double curr, double t) {
    double m0 = 0.5 * (curr - prev2);   /* centered tangent at prev */
    double m1 = (curr - prev);          /* backward tangent at curr */
    double t2 = t * t, t3 = t2 * t;
    double y = (2.0*t3 - 3.0*t2 + 1.0) * prev
             + (t3 - 2.0*t2 + t)       * m0
             + (-2.0*t3 + 3.0*t2)      * curr
             + (t3 - t2)               * m1;
    /* MONOTONE clamp: never leave the [prev,curr] segment box.
     * At a velocity discontinuity (a path-polyline corner - e.g. the
     * titlescreen flyby where one axis stops moving), the centered tangent
     * makes the cubic OVERSHOOT the endpoint and come back - a dip-and-return
     * hitch at every knot (a repeating reversal in the fed position). Clamping is exact for
     * constant-velocity segments (cubic == lerp there), keeps the smooth
     * easing inside the box, and only engages where the overshoot was. */
    double lo = prev < curr ? prev : curr;
    double hi = prev < curr ? curr : prev;
    return y < lo ? lo : (y > hi ? hi : y);
}
/** @brief Rounds v half away from zero and narrows it to int16_t.
 *
 * The caller keeps v inside the int16_t range: hermite1 clamps to the
 * segment between two int16_t samples.
 * @param v The value to round. */
static int16_t hround16(double v) { return (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5); }
/** @brief Rounds v half away from zero and narrows it to int32_t.
 *
 * The caller keeps v inside the int32_t range: hermite1 clamps to the
 * segment between two int32_t samples.
 * @param v The value to round. */
static int32_t hround32(double v) { return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5); }

/** @brief Writes the hermite1 interpolation of nine int16 matrix entries at a byte offset into live.
 *
 * Reads nine consecutive int16_t at off in each of the three snapshot
 * buffers, interpolates entry by entry with hermite1, rounds with
 * hround16, and stores the result at off in live. The offset must be
 * int16-aligned in all four buffers.
 * @param live Receives the interpolated entries.
 * @param prev2 The snapshot before prev.
 * @param prev The segment start snapshot.
 * @param curr The segment end snapshot.
 * @param off Byte offset of the nine entries in each buffer.
 * @param t The sub-tick phase between prev and curr. */
static void herm_matrix(uint8_t* live, const uint8_t* prev2, const uint8_t* prev,
                        const uint8_t* curr, uint32_t off, double t) {
    const int16_t* p2 = (const int16_t*)(prev2 + off);
    const int16_t* pp = (const int16_t*)(prev  + off);
    const int16_t* pc = (const int16_t*)(curr  + off);
    int16_t*       pl = (int16_t*)(live + off);
    for (int i = 0; i < 9; ++i) pl[i] = hround16(hermite1(p2[i], pp[i], pc[i], t));
}
/** @brief Writes the hermite1 interpolation of an int32 x, y, z triple at a byte offset into live.
 *
 * Reads three consecutive int32_t at off in each of the three snapshot
 * buffers, interpolates per axis with hermite1, rounds with hround32, and
 * stores the result at off in live. The offset must be int32-aligned in
 * all four buffers.
 * @param live Receives the interpolated position.
 * @param prev2 The snapshot before prev.
 * @param prev The segment start snapshot.
 * @param curr The segment end snapshot.
 * @param off Byte offset of the triple in each buffer.
 * @param t The sub-tick phase between prev and curr. */
static void herm_vec3(uint8_t* live, const uint8_t* prev2, const uint8_t* prev,
                      const uint8_t* curr, uint32_t off, double t) {
    const int32_t* p2 = (const int32_t*)(prev2 + off);
    const int32_t* pp = (const int32_t*)(prev  + off);
    const int32_t* pc = (const int32_t*)(curr  + off);
    int32_t*       pl = (int32_t*)(live + off);
    for (int i = 0; i < 3; ++i) pl[i] = hround32(hermite1(p2[i], pp[i], pc[i], t));
}

/* ---- fractional interpolated-position table (the lerp32 1-unit floor fix) -----
 * lerp_vec3 writes the interpolated camera/object position to int32 RAM, which
 * ROUNDS to the 1-world-unit grid. Feeders that project against those positions
 * read the rounded RAM and inherit a uniform 1-unit positional floor (sub-pixel,
 * but it caps near-geometry continuity under sub-tick interpolation). For every
 * bracket we ALSO stash the SAME interpolation at DOUBLE precision (un-rounded),
 * keyed by vaddr (G_CAMERA / G_SPYRO / a moby vaddr); openpete_native_interp_pos
 * hands it back so feeders use the smooth path for their camera/object-relative
 * deltas instead of re-reading the floored RAM. Rebuilt every present, valid for
 * the duration of the feeder run (which executes inside reextract). */
/* Must cover every fpos_set caller in one present: camera + Spyro + the moby walk
 * (≤ MOBY_SAVE_MAX) + the HUD icons (G_HUD_COUNT). Undersizing silently drops the
 * fractional position for the overflow mobys -> they fall back to the 1-unit RAM
 * floor (sub-pixel, not corruption), so keep this ≥ MOBY_SAVE_MAX + a little. */
#define FPOS_MAX (MOBY_SAVE_MAX + G_HUD_COUNT + 4)
static struct { uint32_t key; double p[3]; } s_fpos[FPOS_MAX];
static int s_fpos_n = 0;
/* The table is built + consumed entirely within a single sub-tick re-extract
 * present. The feeders are SHARED with the canonical 30 Hz render path, which
 * never refreshes the table - so the resolver MUST report "no entry" there (else
 * the canonical render reads stale fractional positions frozen at the last
 * sub-tick present: terrain/Spyro freeze while moby keyframe animation, a
 * separate channel, keeps going). s_fpos_active is 1 only during the re-extract
 * body; the canonical path sees it 0 and every feeder falls back to live RAM. */
static int s_fpos_active = 0;
static int s_prod_cam_active;   /* a producer's captured camera is installed (defined below) */
/* Key index over s_fpos: open addressing, linear probing, one slot per key
 * naming its FIRST entry (the one a scan from index 0 finds). A slot is
 * live only when its gen equals s_fpos_gen, so fpos_reset empties the
 * index by bumping the generation. FPOS_HASH is a power of two at least
 * twice FPOS_MAX, so a probe always reaches an empty slot. */
#define FPOS_HASH 4096u
static struct { uint32_t key; uint32_t gen; int idx; } s_fpos_hash[FPOS_HASH];
static uint32_t s_fpos_gen = 1;
_Static_assert(FPOS_HASH >= 2u * FPOS_MAX, "fpos index load factor");

/** @brief Home slot of key in the fpos index (Fibonacci hash). */
static uint32_t fpos_hash_slot(uint32_t key) {
    return (uint32_t)(key * 2654435761u) >> 20;   /* top 12 bits: FPOS_HASH = 4096 */
}

/** @brief Index entry idx under key unless key is already indexed (its first entry wins). */
static void fpos_index(uint32_t key, int idx) {
    for (uint32_t h = fpos_hash_slot(key);; h = (h + 1u) & (FPOS_HASH - 1u)) {
        if (s_fpos_hash[h].gen != s_fpos_gen) {
            s_fpos_hash[h].gen = s_fpos_gen; s_fpos_hash[h].key = key; s_fpos_hash[h].idx = idx;
            return;
        }
        if (s_fpos_hash[h].key == key) return;
    }
}

/** @brief Empty the fpos index by advancing its generation (clearing the slots when the counter wraps). */
static void fpos_index_clear(void) {
    if (++s_fpos_gen == 0u) {
        memset(s_fpos_hash, 0, sizeof s_fpos_hash);
        s_fpos_gen = 1;
    }
}

/** @brief Empty the fractional-position table and its index. */
static void fpos_reset(void) { s_fpos_n = 0; fpos_index_clear(); }

/** @brief Append an entry for key and index it; a full table (FPOS_MAX entries) drops it.
 *
 * @return The entry, or NULL when the table is full. */
static double* fpos_append(uint32_t key) {
    if ((unsigned)s_fpos_n >= FPOS_MAX) return NULL;
    s_fpos[s_fpos_n].key = key;
    fpos_index(key, s_fpos_n);
    return s_fpos[s_fpos_n++].p;
}

/** @brief Records key's fractional position as the lerp of prev and curr at t; a full table drops it. */
static void fpos_set(uint32_t key, const int32_t* prev, const int32_t* curr, double t) {
    double* p = fpos_append(key);
    if (!p) return;
    for (int i = 0; i < 3; ++i)
        p[i] = (double)prev[i] + ((double)curr[i] - (double)prev[i]) * t;
}
/** @brief Records key's fractional position as the pred1 Hermite of prev2, prev and curr, the fpos_set variant.
 *
 * Uses hermite1 per axis, the computation herm_vec3 writes to RAM, so the
 * unrounded position feeders read agrees with the staged bytes. A full
 * table (FPOS_MAX entries) drops the entry.
 * @param key The object's guest address.
 * @param prev2 The position one tick before prev.
 * @param prev The prev position.
 * @param curr The curr position.
 * @param t The sub-tick phase. */
static void fpos_set_h(uint32_t key, const int32_t* prev2, const int32_t* prev,
                       const int32_t* curr, double t) {
    double* p = fpos_append(key);
    if (!p) return;
    for (int i = 0; i < 3; ++i)
        p[i] = hermite1((double)prev2[i], (double)prev[i], (double)curr[i], t);
}
/** @brief Returns the fractional-position table index of key.
 *
 * With fpos_drop, fpos_get and fpos_put it lets the Spyro record loop swap
 * the G_SPYRO entry per record and restore the ambient one after its last
 * record.
 * An index probe (fpos_index), so a lookup costs one compare while the
 * table's keys hash apart instead of a scan of every entry.
 * @param key The object's guest address.
 * @return The index of key's first entry, -1 when key has no entry. */
static int fpos_find(uint32_t key) {
    for (uint32_t h = fpos_hash_slot(key);; h = (h + 1u) & (FPOS_HASH - 1u)) {
        if (s_fpos_hash[h].gen != s_fpos_gen) return -1;
        if (s_fpos_hash[h].key == key) return s_fpos_hash[h].idx;
    }
}
/** @brief Remove key's first entry (the last entry moves into its place) and rebuild the index. */
static void fpos_drop(uint32_t key) {
    int i = fpos_find(key);
    if (i < 0) return;
    s_fpos[i] = s_fpos[--s_fpos_n];
    fpos_index_clear();
    for (int k = 0; k < s_fpos_n; ++k) fpos_index(s_fpos[k].key, k);
}
/** @brief Copies key's first fractional-position entry into out.
 *
 * @param key The object's guest address.
 * @param out Receives the x, y, z position; unchanged when key has no entry.
 * @return 1 when key has an entry, else 0. */
static int fpos_get(uint32_t key, double out[3]) {
    int i = fpos_find(key);
    if (i < 0) return 0;
    out[0] = s_fpos[i].p[0]; out[1] = s_fpos[i].p[1]; out[2] = s_fpos[i].p[2];
    return 1;
}
/** @brief Overwrite key's first entry, or append one; a full table drops a new key. */
static void fpos_put(uint32_t key, const double p[3]) {
    int i = fpos_find(key);
    double* d = (i >= 0) ? s_fpos[i].p : fpos_append(key);
    if (!d) return;
    d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
}
/** @brief Return the un-rounded interpolated position of a camera, player or moby the present in flight bracketed (openpete_native_interp_pos). */
static int orch_interp_pos(unsigned int key, double out[3]) {
    if (!s_fpos_active) return 0;   /* canonical render path -> use live RAM */
    if (s_prod_cam_active && key == G_CAMERA) return 0;   /* held producer camera: integer RAM */
    int i = fpos_find(key);
    if (i < 0) return 0;
    out[0] = s_fpos[i].p[0]; out[1] = s_fpos[i].p[1]; out[2] = s_fpos[i].p[2];
    return 1;
}

/* ---- render camera lock (G) ---------------------------------------------------
 * Presentation-side camera override (openpete_camlock.h): while engaged, every
 * present's camera bracket is overwritten with a host-flown pose instead of the
 * interpolated prev/curr pair. The write happens INSIDE the existing bracket
 * (saved before, restored after, covered by the bracket-integrity check), so
 * gameplay RAM is byte-identical lock-on vs lock-off - cutscenes and flybys
 * keep simulating underneath while the view holds still. This is what the
 * gameplay freecam (F) cannot do: its drive runs inside the CameraFollowSpyro
 * intercept, which cutscene cameras never call, and it freezes UpdateSpyro.
 *
 * Matrices are rebuilt with CameraUpdateMatrices' exact construction (camera.c
 * 0x800338B8): view = X(pitch)·Y(yaw) with roll 0 in Q12, proj = view with the
 * Y row scaled 320/512 (the framebuffer-vs-display aspect bake - the reason
 * this matrix is rows-orthogonal but NOT orthonormal). Host lround trig vs the
 * game's table is at most 1 LSB apart - irrelevant for a debug view. The
 * initial pose is NUMERICALLY fitted to the live view matrix rather than read
 * from m_Rotation, because cutscene camera writers may leave the euler fields
 * stale; until the user actually flies, the latched bytes are held VERBATIM so
 * engage-without-input is an exact hold of the on-screen view. */

#define CL_ROT_ROLL_OFF  0x4Cu   /* g_Camera.m_Rotation int16 roll  */
#define CL_ROT_PITCH_OFF 0x4Eu   /*                        pitch */
#define CL_ROT_YAW_OFF   0x50u   /*                        yaw   */

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

static uint8_t s_cl_hold[CAM_BYTES];   /* latched camera bytes (verbatim hold) */
static int     s_cl_have_hold = 0;

/** @brief Computes the Q12 cosine and sine of an angle given in 4096-per-turn units.
 *
 * @param units The angle, 4096 units per turn.
 * @param c Receives the rounded Q12 cosine.
 * @param s Receives the rounded Q12 sine. */
static void cl_trig_q12(double units, int32_t* c, int32_t* s) {
    double r = units * (2.0 * M_PI / 4096.0);
    *c = (int32_t)lround(4096.0 * cos(r));
    *s = (int32_t)lround(4096.0 * sin(r));
}

/** @brief Builds the camera view and projection matrices from yaw and pitch as CameraUpdateMatrices does.
 *
 * view = X(pitch) Y(yaw) with roll 0, in Q12 (camera.c 0x800338B8);
 * proj is view with row 1 scaled by 320/512 in integer arithmetic, the
 * game's expression. Host trigonometry is rounded, at most 1 LSB from the
 * game's table.
 * @param yaw The yaw, 4096 units per turn.
 * @param pitch The pitch, 4096 units per turn.
 * @param view Receives the view matrix, Q12 row-major.
 * @param proj Receives the projection matrix, Q12 row-major. */
static void cl_build_matrices(double yaw, double pitch, int16_t view[9], int16_t proj[9]) {
    int32_t cp, sp, cy, sy;
    cl_trig_q12(pitch, &cp, &sp);
    cl_trig_q12(yaw,   &cy, &sy);
    /* X(pitch) rows: (4096,0,0) (0,cp,-sp) (0,sp,cp);
     * Y(yaw)   rows: (cy,0,sy) (0,4096,0) (-sy,0,cy);  R = X·Y (Q12). */
    const int32_t X[3][3] = {{4096, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    const int32_t Y[3][3] = {{cy, 0, sy}, {0, 4096, 0}, {-sy, 0, cy}};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            int64_t acc = 0;
            for (int k = 0; k < 3; k++) acc += (int64_t)X[r][k] * Y[k][c];
            view[r * 3 + c] = (int16_t)(acc >> 12);
        }
    for (int i = 0; i < 9; i++) proj[i] = view[i];
    for (int c = 0; c < 3; c++) proj[3 + c] = (int16_t)((view[3 + c] * 320) / 512);
}

/** @brief Fits yaw and pitch to a live view matrix by inverting cl_build_matrices on a grid.
 *
 * Three passes: yaw over [0, 4096] and pitch over [-992, 992] in steps of
 * 32, then steps of 4 and 1 within 32 of the best. For a matrix from the
 * game's own euler construction the error is quantisation only (a few
 * hundred at most); a rolled or otherwise built cutscene matrix fits with
 * a large error, and the pose is the nearest roll-free one.
 * @param live The live view matrix, Q12 row-major.
 * @param out_yaw Receives the fitted yaw.
 * @param out_pitch Receives the fitted pitch.
 * @return The summed squared element error of the best fit. */
static double cl_fit_pose(const int16_t* live, double* out_yaw, double* out_pitch) {
    double by = 0.0, bp = 0.0, berr = 1e30;
    for (int pass = 0; pass < 3; pass++) {
        double ys, ye, ps, pe, step;
        if (pass == 0) { ys = 0; ye = 4096; ps = -992; pe = 992; step = 32; }
        else {
            step = (pass == 1) ? 4 : 1;
            ys = by - 32; ye = by + 32; ps = bp - 32; pe = bp + 32;
            if (ps < -992) ps = -992;
            if (pe > 992) pe = 992;
        }
        for (double y = ys; y < ye + 0.5; y += step)
            for (double p = ps; p < pe + 0.5; p += step) {
                int16_t v[9], pr[9];
                cl_build_matrices(y, p, v, pr);
                double err = 0;
                for (int i = 0; i < 9; i++) {
                    double d = (double)v[i] - (double)live[i];
                    err += d * d;
                }
                if (err < berr) { berr = err; by = y; bp = p; }
            }
    }
    *out_yaw = by; *out_pitch = bp;
    return berr;
}

/** @brief Replaces or adds a fractional-position table entry, so feeders read the locked camera position instead of the bracket's lerp.
 *
 * A full table (FPOS_MAX entries) leaves a missing entry unadded.
 * @param key The object's guest address, G_CAMERA from cl_apply.
 * @param x The world X.
 * @param y The world Y.
 * @param z The world Z. */
static void cl_fpos_override(uint32_t key, double x, double y, double z) {
    const double p[3] = { x, y, z };
    fpos_put(key, p);
}

/** @brief Applies the render camera lock to one present's camera bytes.
 *
 * The caller has saved the bytes in its bracket. On the first apply
 * without a pose it fits the pose to the live view matrix (cl_fit_pose),
 * sets it, and latches the bytes. It integrates the freecam axes over
 * wall time in tick units. Until the pose moves, the latched bytes are
 * written back verbatim; after, position, m_Rotation (roll 0) and the
 * view and projection matrices are rebuilt from the pose. The camera's
 * fractional-position entry is set to the pose, and each changed pose is
 * logged in the OPENPETE_CAMLOCK_POSE format.
 * @param cam The staged g_Camera bytes, CAM_BYTES long. */
static void cl_apply(uint8_t* cam) {
    /* First apply after an engage without a scripted pose: latch. */
    if (!openpete_camlock_pose_valid()) {
        const int32_t* pos = (const int32_t*)(cam + CAM_OFF_POS);
        double yaw, pitch;
        double err = cl_fit_pose((const int16_t*)(cam + CAM_OFF_VIEW),
                                 &yaw, &pitch);
        openpete_camlock_set_pose((double)pos[0], (double)pos[1], (double)pos[2],
                                  yaw, pitch);
        memcpy(s_cl_hold, cam, sizeof s_cl_hold);
        s_cl_have_hold = 1;
        fprintf(stderr, "[camlock] latched pos=(%d, %d, %d) yaw=%d pitch=%d fit_err=%.0f%s\n",
                pos[0], pos[1], pos[2], (int)lround(yaw), (int)lround(pitch), err,
                err > 4096.0 ? " (non-euler camera matrix; fly input will snap to the nearest roll-free view)" : "");
    }

    /* Fly: advance the pose from the shared freecam axes at wall-time rate. */
    {
        static op_time_t s_prev_ns = 0;
        op_time_t now = op_now(); /* clock-guard: wall */ /* fly input integrates at wall rate */
        double dt = (s_prev_ns && g_openpete_tick_period_ns > 0)
                        ? (double)(now - s_prev_ns) / (double)g_openpete_tick_period_ns
                        : 0.0;
        s_prev_ns = now;
        openpete_camlock_integrate(dt);
    }

    double x, y, z, yaw, pitch;
    openpete_camlock_get_pose(&x, &y, &z, &yaw, &pitch);

    if (!openpete_camlock_moved() && s_cl_have_hold) {
        /* Untouched since engage: exact hold of the latched view. */
        memcpy(cam, s_cl_hold, sizeof s_cl_hold);
    } else {
        int32_t* pos = (int32_t*)(cam + CAM_OFF_POS);
        pos[0] = (int32_t)lround(x);
        pos[1] = (int32_t)lround(y);
        pos[2] = (int32_t)lround(z);
        int32_t yi = (int32_t)lround(yaw), pi = (int32_t)lround(pitch);
        *(int16_t*)(cam + CL_ROT_ROLL_OFF)  = 0;
        *(int16_t*)(cam + CL_ROT_PITCH_OFF) = (int16_t)(pi & 0xFFF);
        *(int16_t*)(cam + CL_ROT_YAW_OFF)   = (int16_t)(yi & 0xFFF);
        int16_t view[9], proj[9];
        cl_build_matrices(yaw, pitch, view, proj);
        memcpy(cam + CAM_OFF_VIEW, view, sizeof view);   /* pads left as bracketed */
        memcpy(cam + CAM_OFF_PROJ, proj, sizeof proj);
    }

    cl_fpos_override(G_CAMERA, x, y, z);

    /* Round-trip log (change-gated): paste this line into
     * OPENPETE_CAMLOCK_POSE to reproduce the view headlessly. */
    {
        static int32_t l[5] = {0}; static int have = 0;
        int32_t c[5] = { (int32_t)lround(x), (int32_t)lround(y), (int32_t)lround(z),
                         (int32_t)lround(yaw), (int32_t)lround(pitch) };
        if (!have || memcmp(l, c, sizeof c)) {
            have = 1; memcpy(l, c, sizeof c);
            fprintf(stderr, "[camlock] pos=(%d, %d, %d) yaw=%d pitch=%d\n",
                    c[0], c[1], c[2], c[3], c[4]);
        }
    }
}

/* ---- cutscene path-follower helpers -----------------------------------------
 * Cutscene actors (main-EXE func_8002BFE0/func_8002C0EC) ride a packed
 * polyline held in g_CutsceneLayout: one word per waypoint, three 10-bit
 * coords ×256, advanced at idx = t/div with a coarse 64ths lerp between
 * wp[idx] and wp[idx+1]. The actors are the first `count` entries of
 * g_LevelMobys and the decoder writes their position at +0x0C; the camera
 * rides a separate full-precision 24-byte record table at +0x10. */
/** @brief Reads a guest word from canonical guest memory, not through the sub-tick view.
 *
 * @param va The guest address, in [0x80000000, 0x801FFFFC].
 * @param out Receives the word.
 * @return 1 on success, 0 when va is out of range or unmapped. */
static int cut_rd32(uint32_t va, uint32_t* out) {
    if (va < 0x80000000u || va >= 0x80200000u) return 0;
    const uint8_t* p = (const uint8_t*)openpete_psx_to_host_span(va, 4u);
    if (!p) return 0;
    memcpy(out, p, 4);
    return 1;
}

#define G_GAMESTATE_WORD  0x800757D8u  /* g_Gamestate */
#define G_TITLE_MODE      0x80078D78u  /* g_TitlescreenState.m_Mode */
#define GS_TITLESCREEN    13u
#define GS_CUTSCENE       14u
#define TSM_DEMO          3u

/** @brief Reports whether a moby is a cutscene path-follower: one of the first count entries of g_LevelMobys while the sampler func_8002BFE0 runs.
 *
 * The sampler runs under GS_Cutscene (GamestateCutsceneUpdate) and under
 * GS_TitleScreen outside TSM_Demo (TitlescreenUpdate); in any other state
 * g_CutsceneLayout is a stale pointer. count is g_CutsceneLayout +0x0C (the
 * intro has 16 actors), taken only in [1, 64].
 * @param m The moby's guest address.
 * @return 1 for a path-follower, else 0. */
int spyro_cutpath_is_actor(uint32_t m) {
    uint32_t gs, mode, cs, base, cnt;
    if (!cut_rd32(G_GAMESTATE_WORD, &gs)) return 0;
    if (gs == GS_TITLESCREEN) {
        if (!cut_rd32(G_TITLE_MODE, &mode) || mode == TSM_DEMO) return 0;
    } else if (gs != GS_CUTSCENE) {
        return 0;
    }
    if (!cut_rd32(G_CUTSCENE, &cs) || cs < 0x80000000u || cs >= 0x80200000u) return 0;
    if (!cut_rd32(G_LEVELMOBYS, &base) || base < 0x80000000u || base >= 0x80200000u) return 0;
    if (m < base) return 0;
    uint32_t off = m - base;
    if (off % MOBY_STRIDE) return 0;
    if (!cut_rd32(cs + 0x0C, &cnt) || cnt == 0 || cnt > 64u) return 0;
    return (off / MOBY_STRIDE) < cnt;
}

/** @brief The sampler clock m_CurrentTick that func_8002BFE0 wrote into one actor's record.
 *
 * The sampler derives waypoint a and the 64ths fraction b from
 * m_CurrentTick by a divisor K chosen by m_ProgressPerTick of
 * g_Models[1 + idx]->m_Animations[0] (16 -> 8, 21 -> 6, 32 -> 4, else 2),
 * and stores m_Animation = a / 60, m_Frame = a % 60, m_NextAnimation =
 * (a + 1) / 60, m_NextFrame = (a + 1) % 60 and m_FrameProgress = b =
 * (tick % K) * 64 / K. The inverse is tick = a * K + b / (64 / K).
 * @param idx The actor's index in g_LevelMobys.
 * @param rec The actor's moby record.
 * @param tick Receives the clock.
 * @return 1 when the record's animation state is one the sampler writes,
 *         0 when it is not, -1 when the model is unreadable. */
static int cutpath_record_tick(uint32_t idx, const uint8_t* rec, int32_t* tick) {
    uint32_t mdl, anim;
    if (!cut_rd32(G_MODELS + (1u + idx) * 4u, &mdl) || mdl < 0x80000000u) return -1;
    if (!cut_rd32(mdl + 0x38u, &anim) || anim < 0x80000000u) return -1;   /* m_Animations[0] */
    const uint8_t* ppt = (const uint8_t*)openpete_psx_to_host_span(anim + 0x0Cu, 1u);
    if (!ppt) return -1;                                                  /* m_ProgressPerTick */
    const int32_t k = *ppt == 16u ? 8 : *ppt == 21u ? 6 : *ppt == 32u ? 4 : 2;
    const uint8_t* as = rec + game_layout.moby.anim_state_off;
    const int32_t a = (int32_t)as[0] * 60 + as[2];
    const int32_t b = as[4];
    const int32_t q = 64 / k;
    *tick = a * k + b / q;
    return b % q == 0 && b < 64 && as[1] == (a + 1) / 60 && as[3] == (a + 1) % 60;
}

/** @brief The cutscene path sampler's verdict on one step of an actor it drives.
 *
 * func_8002BFE0 interpolates every waypoint segment: from one tick to the
 * next the clock m_CurrentTick advances by g_DeltaTime, which main clamps to
 * [2, 4], or by 2 under TitlescreenUpdate. The clock moves otherwise only by
 * a store: the title loop (1344 or 1530 -> 1172), the title skip (-> 1170),
 * the return to the title (720 or 1168), a cutscene start (0). A step whose
 * clock advance lies outside [2, 4] is therefore a placement the game did not
 * interpolate, and so is a step onto a sampler-written record from one the
 * sampler did not write. The previous record's clock is decoded against the
 * current model table.
 * @param m The moby's guest address.
 * @param prev_rec Tick N-1's moby record.
 * @param curr_rec Tick N's moby record.
 * @return 1 for a jump, 0 for an interpolated step, -1 when the moby is not
 *         a path-follower or tick N's record is not the sampler's. */
int spyro_cutpath_jump(uint32_t m, const uint8_t* prev_rec, const uint8_t* curr_rec) {
    if (!spyro_cutpath_is_actor(m)) return -1;
    uint32_t base;
    if (!cut_rd32(G_LEVELMOBYS, &base)) return -1;
    const uint32_t idx = (m - base) / MOBY_STRIDE;
    int32_t tp = 0, tc = 0;
    if (cutpath_record_tick(idx, curr_rec, &tc) != 1) return -1;
    if (cutpath_record_tick(idx, prev_rec, &tp) != 1) return 1;
    const int32_t dt = tc - tp;
    return dt < 2 || dt > 4;
}

/* ---- transient .s render-pass scratch bracket --------------------------------
 * The moby + Spyro BODY feeders re-derive geometry from PERSISTENT gamestate
 * (g_LevelMobys / g_Spyro / the models) BUT key off a small set of TRANSIENT
 * scratch the .s render pass leaves in g_SonyImage + the PSX scratchpad:
 *   - the moby REGULAR cull records (which mobys passed the cull) + the per-
 *     vertex fog-lit colour scratch                       (g_SonyImage+0x1100..)
 *   - the Spyro per-segment scratch (keyframe stream ptrs + vert counts)
 *                                                          (g_SonyImage+0x1900)
 *   - the shaded-moby (gems/props) Moby* list             (g_SonyImage+0x2400)
 *   - Spyro's projected int16 SXY (the NCLIP cull verdict) (PSX scratchpad)
 * This scratch only lives DURING the canonical render (the .s rebuilds/consumes
 * it each pass); by the time a sub-tick intermediate runs it is gone, so the
 * feeders read zeros and emit NOTHING - the mobys/Spyro vanish on every
 * intermediate and reappear on canonical frames (the flicker).
 *
 * Fix: capture each region at the canonical hook that produces it (proven live
 * there - the canonical feeder reads it at the same point), then on each
 * intermediate restore it (save-live -> write-canon -> run the READ-ONLY feeders
 * -> restore-live, all on the host fiber). Reusing tick-N's cull verdict for the
 * sub-ticks between N and N+1 is correct: the visible set doesn't change within
 * a tick. The geometry still re-projects through the interpolated camera, so
 * mobys/Spyro move smoothly; only the (sub-pixel) cull decision is tick-N's. */
/* ---- Declarative transient-region registry --------------------------------
 * One row per captured transient sub-region. Adding a region = add a backing
 * buffer + one table row (+ a capture call at its producer hook); the engine
 * below handles save-live -> restore-canon -> staged overlay -> restore-live
 * generically.
 *
 * producer - which canonical hook (re)fills the region; freshness is per
 *   producer (see the epoch comment below).
 * phase - SPHASE_ENTER rows restore at bracket entry (before the moby +
 *   shadow feeders). SPHASE_SEG_OVERLAY rows OVERLAP the regular-records row
 *   in PSX space and are overlaid only AFTER those feeders consumed the
 *   records (scratch_bracket_seg_restore) - reproducing the .s's temporal
 *   reuse of [0x1900,..): the moby pass fills cull records there, then the
 *   Spyro pass overwrites them with its seg scratch once
 *   the records are consumed. (A single-shot restore would clobber records
 *   >=14 at 0x1910+, dropping Sparx and distant chests on sub-ticks.)
 *
 * Row storage is PER ROW (canon+live), so PSX-space overlap never aliases in
 * the backing store - overlap is purely a restore-ORDER concern, which the
 * phase column encodes. Live saves are all taken before any restore (one
 * coherent pre-state), so overlapping rows' saved bytes agree and leave()
 * may restore in any order. */
typedef enum { SPROD_MOBYREG = SPYRO_PROD_MOBYREG, SPROD_SPYRO = SPYRO_PROD_SPYRO,
               SPROD_SHADED = SPYRO_PROD_SHADED, SPROD_COUNT } sprod_t;
typedef enum { SPHASE_ENTER = 0, SPHASE_SEG_OVERLAY } sphase_t;

typedef struct {
    const char* name;
    uint32_t    psx_base;    /* absolute PSX address (RAM or scratchpad) */
    uint32_t    len;
    sprod_t     producer;    /* epoch-stamp owner */
    sphase_t    phase;
    uint8_t*    canon;       /* row-owned canonical capture (len bytes) */
    uint8_t*    live;        /* row-owned live save (len bytes) */
    int         captured;    /* canon[] has ever been filled */
    int         open;        /* live[] holds this present's save */
    uint8_t*    stage;       /* view copy for this present, NULL = write guest RAM */
    int         warned;      /* one-shot capture-length-mismatch warning */
    uint8_t*    prev;        /* the capture canon[] retired, tick N-1's on a held cut */
    uint32_t    epoch;       /* canonical epoch of canon[] */
    uint32_t    prev_epoch;  /* canonical epoch of prev[] */
    op_time_t   prev_ref_ns; /* tick stamp of prev[]'s capture */
    op_time_t   ref_ns;      /* tick stamp of canon[]'s capture */
} scratch_region_t;

#define SXY_BASE 0x1F800000u  /* PSX scratchpad (Spyro SXY / NCLIP verdicts) */

static uint8_t s_colour_buf[3][0x500];   /* [0]=canon  [1]=live  [2]=prev */
static uint8_t s_recs_buf[3][0xE00];
static uint8_t s_shaded_buf[3][0x800];
static uint8_t s_sxy_buf[3][0x400];
static uint8_t s_seg_buf[3][0x100];

static scratch_region_t s_scratch_rows[] = {
    /* per-vertex fog-lit colour scratch [0x1100,0x1600) */
    { .name = "colour", .psx_base = G_SCRATCH + 0x1100u, .len = 0x500u, .producer = SPROD_MOBYREG,
      .phase = SPHASE_ENTER, .canon = s_colour_buf[0], .live = s_colour_buf[1], .prev = s_colour_buf[2] },
    /* full REGULAR cull records [0x1600,0x2400) (<=64 @ 0x38) - captured at
     * the MOBY hook while live, BEFORE the Spyro pass reuses 0x1900 */
    { .name = "recs", .psx_base = G_SCRATCH + 0x1600u, .len = 0xE00u, .producer = SPROD_MOBYREG,
      .phase = SPHASE_ENTER, .canon = s_recs_buf[0], .live = s_recs_buf[1], .prev = s_recs_buf[2] },
    /* shaded Moby* list + lit scratch [0x2400,0x2C00) */
    { .name = "shaded", .psx_base = G_SCRATCH + 0x2400u, .len = 0x800u, .producer = SPROD_SHADED,
      .phase = SPHASE_ENTER, .canon = s_shaded_buf[0], .live = s_shaded_buf[1], .prev = s_shaded_buf[2] },
    /* Spyro's projected int16 SXY (the NCLIP cull verdict) - full 1 KB
     * hardware scratchpad */
    { .name = "sxy", .psx_base = SXY_BASE, .len = 0x400u, .producer = SPROD_SPYRO,
      .phase = SPHASE_ENTER, .canon = s_sxy_buf[0], .live = s_sxy_buf[1], .prev = s_sxy_buf[2] },
    /* Spyro per-segment scratch [0x1900,0x1A00) - overlaps recs */
    { .name = "seg", .psx_base = G_SCRATCH + 0x1900u, .len = 0x100u, .producer = SPROD_SPYRO,
      .phase = SPHASE_SEG_OVERLAY, .canon = s_seg_buf[0], .live = s_seg_buf[1], .prev = s_seg_buf[2] },
};
#define SCRATCH_ROWS ((int)(sizeof s_scratch_rows / sizeof s_scratch_rows[0]))
static int s_any_captured = 0;
static int s_bracket_open = 0;   /* enter saved the live scratch this present */
static int s_bracket_hold = 0;   /* this re-extract is a held camera cut */

/* Canonical-render EPOCH. An openpete_frame stamp ("capture is fresh iff its
 * frame stamp == openpete_frame at re-extract") cannot serve: openpete_frame
 * is a VBL counter (2 per game tick) that has always advanced 1-2 by
 * re-extract time, so such a stamp never matches on any present, the scratch
 * bracket is dead and the feeders silently read live scratch
 * + the unstamped SPEC snapshot (stale during pause/portal-load screens). The epoch is instead bumped ONCE per canonical present, from
 * render_paths_commit - the same host-present-loop anchor that latches the
 * canon-drawn mask, guaranteed to run after the canonical render and before any
 * sub-tick re-extract. A capture is fresh iff its producer ran during the LAST
 * canonical present. No canonical present (CdRead stall) -> no bump -> the last
 * capture stays fresh (the world is byte-static; the run.c world-liveness gate
 * bounds long stalls).
 *
 * Freshness is PER PRODUCER: three canonical producers share the RP_MOBY path
 * bit (REGULAR func_8001F798, SPECULAR func_80020F34, SHADED/HUD/text
 * func_80022A2C), and a pause/portal-load screen runs ONLY the SHADED/text one
 * - a single shared stamp would let the text renderer revalidate the whole
 * moby replay. */
static uint32_t s_canon_epoch_now = 1;   /* stamp captures take (present in flight) */
static uint32_t s_canon_epoch_ok  = 0;   /* required stamp for a capture to be fresh */
static uint32_t s_prod_epoch[SPROD_COUNT] = {0};  /* per-producer last-capture stamp:
                                                   * MOBYREG = colour + cull records,
                                                   * SPYRO   = seg + FK + SXY,
                                                   * SHADED  = shaded list (text hook) */

/* Per-producer draw camera: m_ProjectionMatrix, m_ViewMatrix, m_Position
 * ([0x00,0x34) of g_Camera) as they stood when the producer hook fired. The
 * commit runs after the whole canonical draw, so live g_Camera is then the
 * tick-end camera the interp ring snapshots; a producer whose capture differs
 * drew under a camera the bracket cannot interpolate (the game-over draw
 * rebuilds the matrices from the skybox rotation for the cyclorama after the
 * letters and Spyro, so the tick ends under the skybox camera and re-deriving
 * them from it projects them off-screen). Such a producer's feeder reads the
 * captured camera verbatim instead. */
#define CAM_XFORM_BYTES 0x34u
static uint8_t  s_prod_cam[SPROD_COUNT][CAM_XFORM_BYTES];
static uint32_t s_prod_cam_epoch[SPROD_COUNT];
static int      s_prod_cam_hold[SPROD_COUNT];   /* set at commit: capture != tick-end camera */
static uint8_t* s_body_cam = NULL;              /* the camera bytes the feeders read this present */
static uint8_t  s_prod_cam_saved[CAM_XFORM_BYTES];
static int      s_prod_cam_active = 0;

/** @brief Publish a moby's previous record through the sub-tick view for a cut hold.
 *
 * Stages the whole previous record, so position, class, model and animation
 * state all read N-1 through the view, and pins the fractional position table
 * to it. Guest memory is never written.
 * @param m The moby's guest address.
 * @param o The moby's interp pair.
 * @return 1 when staged, 0 when the staging arena refuses the record. */
static int moby_cut_stage(uint32_t m, const openpete_interp_obj_t* o) {
    uint8_t* st = (uint8_t*)openpete_subtick_view_stage(m, MOBY_STRIDE);
    if (!st) return 0;
    uint32_t len = o->blob_len < MOBY_STRIDE ? o->blob_len : MOBY_STRIDE;
    memcpy(st, o->prev, len);
    const int32_t* pp = (const int32_t*)(o->prev + MOBY_POS);
    fpos_set(m, pp, pp, 0.0);
    return 1;
}

/** @brief Whether the sub-tick view holds `moby` at its previous tick on the current present.
 *
 * moby_cut_stage staged its whole previous record, so every byte of it the
 * feeders read is tick N-1's.
 * @param moby the moby's guest address
 * @return 1 when held, 0 otherwise */
int spyro_moby_cut_held(uint32_t moby) {
    return cut_obj_held(moby) == 1;
}

/** @brief Hold this moby's previous tick for the rest of the interval.
 *
 * The shared guard (cut_obj_hold) takes the verdict and moby_cut_stage
 * publishes the previous record.
 * @return 1 when the hold is in force, 0 for the snap (the canonical tick-N
 *         record). */
static int moby_cut_hold(uint32_t m, const openpete_interp_obj_t* o, op_time_t now, int kind) {
    const double a = (double)(now - o->curr_ns) / interp_period_ns(o->prev_ns, o->curr_ns);
    return cut_obj_hold(kind, m, o, MOBY_POS, a, moby_cut_stage);
}

/** @brief The hold a moby record takes before its own lerp: a class change or a reused slot, or a held camera cut it rolled on.
 *
 * A class change, and a pair whose two states are two allocations of the
 * slot (moby_pair_reborn), hold as CUT_CLASS: tick N-1's moby until alpha 1,
 * then tick N's. A held camera cut shows tick N-1 on
 * every record, so a record that rolled at the cut's tick holds as CUT_HELD
 * (cut_held_rolled).
 * @param o The moby's interp pair.
 * @param pcls Tick N-1's class.
 * @param ccls Tick N's class.
 * @return CUT_CLASS, CUT_HELD, or -1 for no hold. */
static int moby_cut_kind(const openpete_interp_obj_t* o, uint16_t pcls, uint16_t ccls) {
    if (pcls != ccls || moby_pair_reborn(o)) return CUT_CLASS;
    return cut_held_rolled(o) == 1 ? CUT_HELD : -1;
}

/** @brief A record's own alpha, or 1 under a held camera cut for a record that did not roll at the cut's tick.
 *
 * Such a record's current state is its state at tick N-1 (cut_held_rolled).
 * @param o The moby's interp pair.
 * @param ma The record's own alpha.
 * @return The alpha to show. */
static double moby_held_cut_alpha(const openpete_interp_obj_t* o, double ma) {
    return cut_held_rolled(o) == 0 ? 1.0 : ma;
}

/** @brief Whether a record's step is a jump the record holds at tick N-1 (cut_obj_jump).
 *
 * Under a held camera cut a record that did not roll shows its current state
 * whatever its last step was, so no step of it holds.
 * @param m The moby's guest address.
 * @param o The moby's interp pair.
 * @return 1 for a jump to hold, else 0. */
static int moby_step_jumps(uint32_t m, const openpete_interp_obj_t* o) {
    return cut_held_rolled(o) < 0 && cut_obj_jump(m, o->prev, o->curr);
}

/** @brief Returns the g_Camera region index and camera field offsets for the shared cut guard.
 *
 * The region indices resolve on the first call.
 * @return The descriptor, static storage. */
static const cut_cam_desc_t* cut_desc(void) {
    static cut_cam_desc_t d = { -2, CAM_OFF_POS, CAM_OFF_PROJ };
    if (d.cam_region == -2) d.cam_region = CAM_REGION();
    return &d;
}

/** @brief Logs every live moby whose prev/curr pair the cut guard would treat as a cut, once per canonical pass.
 *
 * The guards run on intermediate presents, which a headless run produces
 * only where a capture asks for one. The locator evaluates the guard's
 * predicate (class change at +0x36, or a position jump, cut_obj_jump)
 * against the same object snapshots, so one replay names the ticks at
 * which the moby guards fire. Lines carry `locate=` and the pair's age in
 * ticks; they state nothing about what was presented. The walk stops at
 * state 0xFF and skips states from 0x80. */
static void cut_locate_mobys(void) {
    uint8_t* base_p = (uint8_t*)openpete_psx_to_host_span(G_LEVELMOBYS, 4u);
    uint32_t m = 0; if (base_p) memcpy(&m, base_p, 4);
    if (m < 0x80000000u) return;
    for (int guard = 0; guard < 4096; guard++, m += MOBY_STRIDE) {
        uint8_t* mh = (uint8_t*)openpete_psx_to_host_span(m, MOBY_STRIDE);
        if (!mh || mh[MOBY_STATE] == 0xFF) break;
        if (mh[MOBY_STATE] >= 0x80) continue;
        const openpete_interp_obj_t* o = openpete_interp_objs_find(m);
        if (!o || !o->have_prev) continue;
        uint16_t pcls, ccls;
        memcpy(&pcls, o->prev + 0x36u, 2);
        memcpy(&ccls, o->curr + 0x36u, 2);
        int32_t pp[3], cp[3];
        memcpy(pp, o->prev + MOBY_POS, 12);
        memcpy(cp, o->curr + MOBY_POS, 12);
        double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
        const double d2 = dx*dx + dy*dy + dz*dz;
        const char* kind = (pcls != ccls) ? "class" : (cut_obj_jump(m, o->prev, o->curr) ? "teleport" : NULL);
        if (!kind) continue;
        /* Pair age in ticks: the guard only has a previous state to hold while
         * this object's own interval is still open (age below 1); an object
         * whose curr stopped rolling shows curr whatever the guard decides. */
        double age = (o->prev_ns != 0 && o->curr_ns > o->prev_ns)
                   ? (double)(openpete_interp_present_now() - o->curr_ns)
                     / interp_period_ns(o->prev_ns, o->curr_ns) : 99.0;
        fprintf(stderr, "[cut-census vbl=%u] locate=%s obj=%08x cls=%03x->%03x |d|=%.0f age=%.2f\n",
                openpete_frame, kind, m, (unsigned)pcls, (unsigned)ccls, sqrt(d2), age);
    }
}

/** @brief Record the camera transform block a producer drew under and stamp it with the in-flight epoch (spyro_native_canon_capture_camera). */
static void orch_capture_camera(void* cpu_v, int producer) {
    (void)cpu_v;
    if (producer < 0 || producer >= SPROD_COUNT) return;
    const uint8_t* h = (const uint8_t*)openpete_psx_to_host_span(G_CAMERA, CAM_XFORM_BYTES);
    if (!h) return;
    memcpy(s_prod_cam[producer], h, CAM_XFORM_BYTES);
    s_prod_cam_epoch[producer] = s_canon_epoch_now;
}

/** @brief Install a producer's captured camera over the re-extract's camera when the producer drew under another (spyro_native_prod_camera_enter). */
static int orch_prod_camera_enter(int producer) {
    if (producer < 0 || producer >= SPROD_COUNT) return 0;
    if (!s_body_cam || s_prod_cam_active || s_bracket_hold) return 0;
    if (s_prod_cam_epoch[producer] != s_canon_epoch_ok || !s_prod_cam_hold[producer]) return 0;
    memcpy(s_prod_cam_saved, s_body_cam, CAM_XFORM_BYTES);
    memcpy(s_body_cam, s_prod_cam[producer], CAM_XFORM_BYTES);
    s_prod_cam_active = 1;
    return 1;
}

/** @brief Restore the camera orch_prod_camera_enter replaced (spyro_native_prod_camera_leave). */
static void orch_prod_camera_leave(void) {
    if (!s_prod_cam_active) return;
    memcpy(s_body_cam, s_prod_cam_saved, CAM_XFORM_BYTES);
    s_prod_cam_active = 0;
}

/** @brief Latch which producers drew under another camera, latch the in-flight epoch as fresh, start the next and close the player hooks' record epoch (spyro_native_canon_epoch_commit). */
static void orch_epoch_commit(void) {
    if (cut_census_on()) { cut_locate_camera(cut_desc()); cut_locate_mobys(); }
    const uint8_t* h = (const uint8_t*)openpete_psx_to_host_span(G_CAMERA, CAM_XFORM_BYTES);
    for (int p = 0; p < SPROD_COUNT; ++p)
        s_prod_cam_hold[p] = h && s_prod_cam_epoch[p] == s_canon_epoch_now &&
                             memcmp(s_prod_cam[p], h, CAM_XFORM_BYTES) != 0;
    s_canon_epoch_ok = s_canon_epoch_now;
    s_canon_epoch_now++;
    player_records_epoch_commit();               /* close the hooks' record epoch */
}
/** @brief Move the in-flight epoch past the captures of a render no canonical present showed (spyro_native_canon_epoch_supersede). */
static void orch_epoch_supersede(void) { s_canon_epoch_now++; }
/** @brief Return the in-flight canonical-render epoch (spyro_native_canon_epoch_stamp). */
static uint32_t orch_epoch_stamp(void) { return s_canon_epoch_now; }
/** @brief Report whether a capture stamp is the last closed epoch (spyro_native_canon_epoch_fresh). */
static int orch_epoch_fresh(uint32_t stamp) { return stamp == s_canon_epoch_ok; }
/** @brief Reports whether producer p captured during the last closed canonical render.
 *
 * Compares the producer's last-capture stamp with the last closed epoch,
 * the test orch_mobyreg_fresh and orch_shaded_fresh apply to their own
 * producers. */
static int scratch_prod_fresh(sprod_t p)   { return s_prod_epoch[p] == s_canon_epoch_ok; }

/** @brief The bytes a re-extract restores for a scratch row, tick N-1's on a held cut.
 *
 * Outside a hold, canon[] when the producer ran in the last closed render. On
 * a hold, the capture cut_held_set_pick names, so a queue tick N-1 drew shows
 * whatever tick N's cull kept.
 * @param r The row.
 * @return The bytes, or NULL when the row shows no capture. */
static const uint8_t* scratch_row_shown(const scratch_region_t* r) {
    if (!r->captured) return NULL;
    if (!s_bracket_hold) return scratch_prod_fresh(r->producer) ? r->canon : NULL;
    const cut_held_pick_t pick = cut_held_set_pick(r->epoch, r->prev_epoch);
    return pick == CUT_HELD_PREV ? r->prev : pick == CUT_HELD_CURR ? r->canon : NULL;
}

/** @brief Whether a producer's rows show a capture on this re-extract.
 *
 * @param p The producer.
 * @return 1 when its first row has bytes to show, else 0. */
static int scratch_prod_shown(sprod_t p) {
    for (int i = 0; i < SCRATCH_ROWS; ++i)
        if (s_scratch_rows[i].producer == p) return scratch_row_shown(&s_scratch_rows[i]) != NULL;
    return 0;
}

/** @brief Report whether the moby-record producer has a capture to show (spyro_native_canon_mobyreg_fresh). */
static int orch_mobyreg_fresh(void) { return scratch_prod_shown(SPROD_MOBYREG); }
/** @brief Report whether the shaded producer has a capture to show (spyro_native_canon_shaded_fresh). */
static int orch_shaded_fresh(void)  { return scratch_prod_shown(SPROD_SHADED); }

/* Registry engine: row lookup + capture. Captures fire at the producer hooks
 * (canonical moby/spyro/shaded render hooks, while the scratch is still live)
 * and stamp the row's producer epoch. */
/** @brief Returns the scratch registry row whose base address is psx_addr.
 *
 * @param psx_addr The guest base address.
 * @return The row, NULL when no row starts there. */
static scratch_region_t* scratch_row_by_base(uint32_t psx_addr) {
    for (int i = 0; i < SCRATCH_ROWS; ++i)
        if (s_scratch_rows[i].psx_base == psx_addr) return &s_scratch_rows[i];
    return NULL;
}

/** @brief Copies a scratch registry row's live bytes into its canon buffer and stamps the producer epoch.
 *
 * The epoch's first capture retires canon[] to prev[]; a later capture in the
 * epoch replaces canon[]. */
static void scratch_row_capture(scratch_region_t* r) {
    const void* h = openpete_psx_to_host_span(r->psx_base, r->len);
    if (!h) return;
    if (r->epoch != s_canon_epoch_now) {
        uint8_t* t = r->prev; r->prev = r->canon; r->canon = t;
        r->prev_epoch = r->epoch;
        r->prev_ref_ns = r->ref_ns;
        r->epoch = s_canon_epoch_now;
    }
    r->ref_ns = atomic_load_explicit(&g_openpete_tick_stamp, memory_order_relaxed);
    memcpy(r->canon, h, r->len);
    r->captured = 1;
    s_any_captured = 1;
    s_prod_epoch[r->producer] = s_canon_epoch_now;
    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_CANON_CAPDBG"); dbg = (e && *e && *e != '0'); }
    if (dbg && (openpete_frame % 200u) == 0) {
        uint32_t w0; memcpy(&w0, r->canon, 4);
        fprintf(stderr, "[canon-cap f=%u] row=%s base=0x%08x len=0x%x word0=%08x\n",
                openpete_frame, r->name, r->psx_base, r->len, w0);
    }
}

/** @brief Capture the scratch registry row at G_SCRATCH + off; an offset no row starts at logs and captures nothing (spyro_native_canon_capture_range). */
static void orch_capture_range(void* cpu_v, unsigned off, unsigned len) {
    (void)cpu_v;
    scratch_region_t* r = scratch_row_by_base(G_SCRATCH + off);
    if (!r) {
        fprintf(stderr, "[canon-cap] UNMAPPED capture offset 0x%04x (add a registry row)\n", off);
        return;
    }
    if (len != r->len && !r->warned) {
        r->warned = 1;
        fprintf(stderr, "[canon-cap] row '%s': capture len 0x%x != row len 0x%x (row len used)\n",
                r->name, len, r->len);
    }
    scratch_row_capture(r);
}

/** @brief Capture the registry row at SXY_BASE, the PSX scratchpad (spyro_native_canon_capture_sxy). */
static void orch_capture_sxy(void* cpu_v) {
    (void)cpu_v;
    scratch_row_capture(scratch_row_by_base(SXY_BASE));
}

/** @brief Capture the whole regular cull-record region [0x1600,0x2400) at the moby hook (spyro_native_canon_capture_recs).
 *
 * The records are still live there; the later Spyro pass reuses 0x1900 for
 * its seg scratch, whose own row is captured at the Spyro hook. */
static void orch_capture_recs(void* cpu_v) {
    (void)cpu_v;
    scratch_row_capture(scratch_row_by_base(G_SCRATCH + 0x1600u));
}

/* On an intermediate: save the live scratch, overwrite with the captured canon,
 * so the read-only feeders that follow see valid tick-N render-pass inputs.
 *
 * The seg scratch [0x1900,0x1A00) is NOT restored here - it overlaps the regular
 * records (records 14+ start at 0x1910) which the .s reuses temporally. We give
 * the moby feeder the FULL records first; scratch_bracket_seg_restore then
 * overlays the seg scratch for the Spyro feeder (called between them). */

/** @brief Open a held cut's background bracket: tick N-1's clear, except over a world the load has taken.
 *
 * That clear alone is a flat screen tick N-1 never showed, so such a present
 * keeps tick N's.
 * @param cpu The guest CPU state.
 * @param hold Nonzero on a held camera cut. */
static void held_background_open(CPUState* cpu, int hold) OP_REQUIRES(sim) {
    cyclorama_native_hold_background(hold && !terrain_native_held_world_lost(cpu));
}

/** @brief The retired regular records, the queue a held cut replays (spyro_native_canon_prev_recs). */
static const uint8_t* orch_prev_recs(uint32_t* len, op_time_t* ref_ns) {
    const scratch_region_t* r = scratch_row_by_base(G_SCRATCH + 0x1600u);
    if (!r || !r->captured || cut_held_set_pick(r->epoch, r->prev_epoch) != CUT_HELD_PREV) return NULL;
    *len = r->len;
    *ref_ns = r->prev_ref_ns;
    return r->prev;
}

/** @brief Saves every scratch row's live bytes, stages each row through the view, and restores fresh producers' canon captures into it. */
static void scratch_bracket_enter(CPUState* cpu, int hold) {
    (void)cpu;
    s_bracket_hold = hold;
    moby_native_held_scratch(hold);
    {   /* OPENPETE_LOAD_CENSUS: per-producer freshness at re-extract time. */
        static int cen = -1;
        if (cen < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); cen = (e && *e && *e != '0'); }
        static unsigned last = 0xFFFFFFFFu;
        if (cen && openpete_frame != last) { last = openpete_frame;
            fprintf(stderr, "[census f=%u BRACKET] epoch_ok=%u fresh reg=%d spy=%d shd=%d\n",
                    openpete_frame, s_canon_epoch_ok,
                    spyro_native_canon_mobyreg_fresh(),
                    scratch_prod_fresh(SPROD_SPYRO),
                    spyro_native_canon_shaded_fresh()); }
    }
    if (!s_any_captured) return;
    /* Save TRUE live for ALL rows first, before any restore - one coherent
     * pre-state, so an overlapping row never saves another row's canon bytes,
     * and leave() restores everything back whether or not a canon restore
     * fired (a stale-skipped region is a saved-then-restored no-op). */
    for (int i = 0; i < SCRATCH_ROWS; ++i) {
        scratch_region_t* r = &s_scratch_rows[i];
        void* h = openpete_psx_to_host_span(r->psx_base, r->len);
        if (!h) continue;
        memcpy(r->live, h, r->len);
        r->open = 1;
        /* Publish this row's replay through the view. Staging seeds from the
         * live bytes, so a row whose producer is stale keeps exactly what it
         * would have kept before -- the canon overwrite below is what makes a
         * fresh row differ, same as when it went to guest memory.
         *
         * Overlapping rows resolve correctly by construction: a SEG_OVERLAY row
         * lands inside the span its records row already staged, so stage()
         * returns that same buffer and the overlay writes into it in phase
         * order -- reproducing the temporal reuse the .s renderer performs,
         * which is the whole reason the phase column exists. */
        r->stage = (uint8_t*)openpete_subtick_view_stage(r->psx_base, r->len);
    }
    s_bracket_open = 1;
    /* Restore canon per PRODUCER freshness: a producer that didn't run during
     * the last canonical present keeps whatever the live scratch holds (its
     * feeder pass is skipped too - moby_reextract checks the same
     * predicates), instead of replaying a stale capture. SEG_OVERLAY rows
     * wait for scratch_bracket_seg_restore (they overlap the records the
     * moby + shadow feeders are about to consume). */
    for (int i = 0; i < SCRATCH_ROWS; ++i) {
        scratch_region_t* r = &s_scratch_rows[i];
        if (r->phase != SPHASE_ENTER || !r->open) continue;
        const uint8_t* src = scratch_row_shown(r);
        if (!src) continue;
        if (r->stage) { memcpy(r->stage, src, r->len); continue; }
        void* h = openpete_psx_to_host_span(r->psx_base, r->len);
        if (h) memcpy(h, src, r->len);
    }
}

/** @brief Overlays the captured SEG_OVERLAY scratch rows over the consumed cull records.
 *
 * Runs after the moby and shadow feeders have read the regular records,
 * and writes the rows at 0x1900 onward as the .s reuses that memory, so
 * the Spyro body feeder reads its seg scratch instead of record bytes. Only rows captured by a
 * producer that ran in the last canonical present are written, into the
 * staged copy when one exists, else into guest memory. No-op when the
 * bracket is not open.
 * @param cpu Unused. */
static void scratch_bracket_seg_restore(CPUState* cpu) {
    (void)cpu;
    if (!s_bracket_open) return;
    for (int i = 0; i < SCRATCH_ROWS; ++i) {
        scratch_region_t* r = &s_scratch_rows[i];
        if (r->phase != SPHASE_SEG_OVERLAY || !r->open) continue;
        const uint8_t* src = scratch_row_shown(r);
        if (!src) continue;
        if (r->stage) { memcpy(r->stage, src, r->len); continue; }
        void* h = openpete_psx_to_host_span(r->psx_base, r->len);
        if (h) memcpy(h, src, r->len);
    }
}

/** @brief Restores the live scratch saved by scratch_bracket_enter before the game fiber resumes.
 *
 * Every open row is closed. A staged row was never written to guest
 * memory and needs no restore; any other row gets its saved bytes back.
 * Overlapping rows all saved the same pre-state, so restore order does
 * not matter. No-op when the bracket is not open.
 * @param cpu Unused. */
static void scratch_bracket_leave(CPUState* cpu) {
    (void)cpu;
    s_bracket_hold = 0;
    moby_native_held_scratch(0);
    if (!s_bracket_open) return;
    s_bracket_open = 0;
    for (int i = 0; i < SCRATCH_ROWS; ++i) {
        scratch_region_t* r = &s_scratch_rows[i];
        if (!r->open) continue;
        r->open = 0;
        if (r->stage) { r->stage = NULL; continue; }  /* never written; nothing to undo */
        void* h = openpete_psx_to_host_span(r->psx_base, r->len);
        if (h) memcpy(h, r->live, r->len);
    }
}

/* ---- the Spyro record loop ----------------------------------------------
 * One Spyro feeder pass per model record of the committed epoch
 * (renderer/spyro_records.c). The ambient bracket above staged the ring's
 * lerped g_Spyro (player 1, the modless singleton) for the feeders that read
 * G_SPYRO per present (the moby feeder's gem gates); this loop restages
 * record k's bytes, its position / anim-clock / rotation brackets against
 * the record it paired from, its scratch rows (seg, SXY)
 * and its tag slot, runs the feeder, and after the last record restores the
 * ambient bytes and fractional-position entry. No feeder after this loop
 * reads G_SPYRO. Modless the list holds one drawn record whose bytes are the
 * ring's (0 mismatches measured), so k=0 is the singleton bracket exactly.
 *
 * Shadow records: after a record's body, every shadow record bound to
 * it restages its own D_SPYROSHADOW bytes over the ambient shadow bracket -
 * the anchor lerped against the previous list's shadow of the paired model,
 * teleport-guarded like the singleton - and runs the Spyro shadow feeder
 * under the record's staged g_Spyro (heading, silhouette selectors, the
 * lerped +0x24 clock). A pairing-only model record (hidden player, shadow
 * drawn) stages its bytes for its shadows and skips the body.
 *
 * `sp` is the ambient staged copy of the whole g_Spyro region (NULL when the
 * view refused staging and the bracket wrote guest RAM: then this loop writes
 * and restores guest RAM the same way, integrity-checked); `sh` the same for the
 * shadow region. `psp_ring` is the ring's prev g_Spyro bytes, for the
 * runahead input-edge rebase (the record whose prev bytes are the ring's
 * prev takes the shown position the same way the singleton did). */
static double s_rec_alpha = 0.0;   /* the store's alpha this present (player_records_present_begin) */

/** @brief Writes a record's bytes over addr through the staged view, else into guest memory that scratch_bracket_leave restores. */
static void spyro_record_overlay(uint32_t addr, const uint8_t* bytes, uint32_t len) {
    uint8_t* h = (uint8_t*)openpete_subtick_view_stage(addr, len);
    if (!h) h = (uint8_t*)openpete_psx_to_host_span(addr, len);   /* scratch_bracket_leave restores the rows */
    if (h) memcpy(h, bytes, len);
}

/** @brief Run the Spyro shadow feeder once per drawn shadow record bound to model record `k`.
 *
 * Each record is staged from its shown state (`src`, the led tick under
 * --runahead) with the ground anchor lerped against the paired record,
 * teleport-guarded; `shb` is the staged shadow span. */
static void player_records_shadows(CPUState* cpu, int k, uint8_t* shb) {
    int nsh = player_records_shadow_count();
    for (int j = 0; j < nsh; j++) {
        const player_shadow_rec_t* d = player_records_shadow_curr(j);
        if (!d || d->model != k || !d->drawn) continue;
        const player_shadow_rec_t* pd = player_records_shadow_prev(j);
        memcpy(shb, d->src, PLAYER_L.shadow_len);
        if (player_records_lerps(k) && pd) {
            /* The ground anchor (m_Position @ +0x10), teleport-guarded like
             * the singleton bracket (a warp, a lift-off -> re-ground). */
            const int32_t* pp = (const int32_t*)(pd->src + SHADOW_POS_OFF);
            const int32_t* cp = (const int32_t*)(d->src + SHADOW_POS_OFF);
            double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
            if (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0)
                lerp_vec3(shb, pd->src, d->src, SHADOW_POS_OFF, s_rec_alpha);
        }
        shadow_spyro_reextract(cpu, k);
        player_records_shadow_note(j, (const int32_t*)(shb + SHADOW_POS_OFF));
    }
}

/** @brief One Spyro feeder pass per model record of the committed epoch, then its shadow records.
 *
 * Each record is staged from its shown state (`src`, the led tick under
 * --runahead) with position, anim clock and rotation bracketed against the
 * record it paired from. Restores the ambient g_Spyro and shadow bytes after
 * the last record. */
static void player_records_loop(CPUState* cpu, int paired, int cam_cut, int cam_bracketed,
                               double alpha, int use_herm,
                               uint8_t* sp, int sp_staged, const uint8_t* psp_ring,
                               uint8_t* sh) {
    double rec_alpha = 0.0;
    int n = player_records_present_begin(paired, cam_cut, alpha, &rec_alpha);
    if (n <= 0) { player_records_present_end(); return; }
    s_rec_alpha = rec_alpha;

    /* On an unpaired present the ambient brackets did not run and staged
     * nothing: stage the spans here (the view is open for the whole present),
     * guest RAM only if the view refuses. */
    static uint32_t s_present_serial = 0;
    player_native_set_flat_key((++s_present_serial << 1) | 1u);   /* this present's records share the flatten cap */
    uint8_t* sb = sp ? sp : (uint8_t*)openpete_subtick_view_stage(G_SPYRO, PLAYER_L.rec_len);
    if (!sb) sb = (uint8_t*)openpete_psx_to_host_span(G_SPYRO, PLAYER_L.rec_len);
    if (!sb) { player_records_present_end(); return; }
    static uint8_t ambient[PLAYER_REC_STATE_MAX];
    memcpy(ambient, sb, PLAYER_L.rec_len);
    double amb_f[3]; const int amb_f_ok = fpos_get(G_SPYRO, amb_f);
    static const int frA[3] = {0x1E,0x20,0x22}, frB[3] = {0x1F,0x21,0x23}, prog[3] = {0x24,0x25,0x26};
    uint8_t* shb = sh ? sh : (uint8_t*)openpete_subtick_view_stage(D_SPYROSHADOW, PLAYER_L.shadow_len);
    if (!shb) shb = (uint8_t*)openpete_psx_to_host_span(D_SPYROSHADOW, PLAYER_L.shadow_len);
    static uint8_t sh_ambient[PLAYER_REC_AUX_MAX];
    const int nsh = shb ? player_records_shadow_count() : 0;
    if (nsh) memcpy(sh_ambient, shb, PLAYER_L.shadow_len);

    for (int k = 0; k < n; k++) {
        const player_model_rec_t* rec = player_records_curr(k);
        if (!rec) break;
        int has_shadow = 0;
        for (int j = 0; j < nsh; j++) { const player_shadow_rec_t* d = player_records_shadow_curr(j); if (d && d->model == k && d->drawn) { has_shadow = 1; break; } }
        if (!rec->drawn && !has_shadow) { player_records_present_note(k, (const int32_t*)rec->src); continue; }
        const player_model_rec_t* prv = player_records_prev(k);
        const player_model_rec_t* pv2 = player_records_prev2(k);
        const int lerp = player_records_lerps(k) && prv != NULL;

        memcpy(sb, rec->src, PLAYER_L.rec_len);
        fpos_drop(G_SPYRO);
        /* player_native_set_rot_interp reads PLAYER_PARTS_MAX gates and
         * angle triples regardless of how many the loop below fills, so both
         * arrays carry that many entries. The trailing ones stay zeroed, and
         * a zero gate leaves that part on the byte-exact path. */
        int    rot_active[PLAYER_PARTS_MAX] = {0};
        double rot_ang[PLAYER_PARTS_MAX][3] = {{0}};
        if (lerp) {
            const uint8_t* psp = prv->src;
            const uint8_t* csp = rec->src;
            static uint8_t s_psp_rb[PLAYER_REC_STATE_MAX];
            if (openpete_ra_input_edge && ra_rebase_on() && s_shown_sp_ok &&
                !openpete_interp_ra_commit_active() && psp_ring &&
                memcmp(psp, psp_ring, 12) == 0) {
                /* Led-camera A/B config only (see the singleton bracket). */
                memcpy(s_psp_rb, psp, PLAYER_L.rec_len);
                memcpy(s_psp_rb, s_shown_sp, 12);
                psp = s_psp_rb;
            }
            /* Position: pred1 over the record it paired from two ticks back,
             * gated like the singleton (camera continuity + this record's own
             * prev2->prev step). */
            int herm = 0;
            if (use_herm && pv2) {
                const int32_t* p2 = (const int32_t*)pv2->src;
                const int32_t* pp = (const int32_t*)psp;
                double dx=(double)pp[0]-p2[0], dy=(double)pp[1]-p2[1], dz=(double)pp[2]-p2[2];
                herm = (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0);
            }
            if (herm) {
                herm_vec3(sb, pv2->src, psp, csp, 0, rec_alpha);
                fpos_set_h(G_SPYRO, (const int32_t*)pv2->src, (const int32_t*)psp, (const int32_t*)csp, rec_alpha);
            } else {
                lerp_vec3(sb, psp, csp, 0, rec_alpha);
                fpos_set(G_SPYRO, (const int32_t*)psp, (const int32_t*)csp, rec_alpha);
            }
            /* Per-segment animation clock: lerp m_FrameProgress when the
             * keyframe pair is unchanged, else snap. */
            for (int s = 0; s < 3; s++) {
                if (psp[frA[s]] == csp[frA[s]] && psp[frB[s]] == csp[frB[s]]) {
                    int pa = psp[prog[s]], ca = csp[prog[s]];
                    int la = (int)((double)pa + ((double)ca - pa) * rec_alpha + 0.5);
                    if (la < 0) la = 0; else if (la > 255) la = 255;
                    sb[prog[s]] = (uint8_t)la;
                }
            }
            /* Body/head/tail rotation at float precision (never written back
             * to the byte euler). Head/tail carry a 0-sentinel. */
            for (int s = 0; s < 3; s++) {
                uint32_t off = 0x0Cu + (uint32_t)s * 4u;
                uint32_t pr, cr; memcpy(&pr, psp + off, 4); memcpy(&cr, csp + off, 4);
                if (s != 0 && (pr == 0 || cr == 0)) continue;
                for (int a = 0; a < 3; a++) {
                    int pbk = (int)((pr >> (a*8)) & 0xFFu), cbk = (int)((cr >> (a*8)) & 0xFFu);
                    int d = cbk - pbk; if (d > 128) d -= 256; else if (d < -128) d += 256;
                    rot_ang[s][a] = (double)pbk + (double)d * rec_alpha;
                }
                rot_active[s] = 1;
            }
            if (psp_ring && memcmp(prv->src, psp_ring, 12) == 0) {   /* the ring's dragon: shown m_Position (runahead rebase) */
                memcpy(s_shown_sp, sb, 12); s_shown_sp_ok = 1;
            }
        }
        if (rec->drawn) {
            player_native_set_rot_interp(lerp ? rot_active : NULL, &rot_ang[0][0]);
            player_native_set_anim_interp(rec_alpha, lerp && cam_bracketed);
            player_native_set_anim_pair((lerp && prv->drawn) ? prv->seg : NULL, rec->seg);
            player_native_set_player_slot(k);
            /* The rows this call rendered from, over the scratch bracket's staged
             * copies (the bracket staged the same spans; the seg row overlays the
             * consumed records at 0x1900 like the .s does). */
            spyro_record_overlay(PLAYER_L.part_base + PLAYER_L.part_off, rec->seg, PLAYER_L.part_block_len);
            spyro_record_overlay(PLAYER_L.sxy_base, rec->sxy, PLAYER_L.sxy_len);

            spyro_native_prod_camera_enter(SPROD_SPYRO);   /* Spyro's own draw camera when the tick ended under another */
            player_reextract(cpu);            /* interpolated camera + this record's bracket */
            spyro_native_prod_camera_leave();
        }
        player_records_present_note(k, (const int32_t*)sb);
        if (has_shadow) player_records_shadows(cpu, k, shb);
    }

    /* Restore the ambient bytes and entry (player 1 via the ring). */
    memcpy(sb, ambient, PLAYER_L.rec_len);
    fpos_drop(G_SPYRO);
    if (amb_f_ok) fpos_put(G_SPYRO, amb_f);
    if (nsh) memcpy(shb, sh_ambient, PLAYER_L.shadow_len);
    (void)sp_staged;
    player_records_present_end();
}


/** @brief Reports whether the camera snapshots of two ticks are not one camera motion.
 *
 * cut_camera_reason decides from the camera's position and rotation steps
 * over the three ring snapshots. OPENPETE_CAMCUT_DBG logs up to four lines a
 * second of the guard's inputs.
 * @param prev2 The snapshot before prev, or NULL.
 * @param prev The earlier snapshot.
 * @param curr The later snapshot.
 * @param pair_age The pair's age, logged only.
 * @return 1 for a cut, else 0. */
static int camera_cut(const openpete_interp_snapshot_t* prev2,
                      const openpete_interp_snapshot_t* prev,
                      const openpete_interp_snapshot_t* curr, double pair_age) {
    cut_cam_metrics_t m;
    const char* why = cut_camera_reason(prev2, prev, curr, cut_desc(), &m);
    /* OPENPETE_CAMCUT_DBG: rate-limited census of the guard's inputs. A cut
     * costs both the sub-tick camera bracket and the occlusion cull, so a
     * view that reads as cut every present needs its span and delta named. */
    {
        static int s_ccd = -1;
        if (s_ccd < 0) { const char* e = getenv("OPENPETE_CAMCUT_DBG"); s_ccd = (e && *e && *e != '0'); }
        if (s_ccd) {
            static uint64_t t0 = 0; static int n = 0;
            uint64_t now = openpete_phase_now_us();
            if (now - t0 >= 1000000u) { t0 = now; n = 0; }
            if (n < 4) { n++;
                fprintf(stderr, "[camcut f=%u] cut=%d why=%s age=%.1f |d|=%.0f rot=%.1f prot=%.1f "
                        "jerk=%.0f prevcut=%d ptick=%u ctick=%u\n",
                        openpete_frame, why != NULL, why ? why : "-", pair_age,
                        m.step, m.rot, m.prev_rot, m.jerk, m.prev_cut,
                        prev->tick, curr->tick); }
        }
    }
    return why != NULL;
}

/* The bracket's second walk: the mobys the canonical render drew from outside
 * the moby array, then the owners of the live glow records, without repeats.
 * A repeat would be saved after its first lerp and restored over the original. */
#define EXTRA_DRAWN_MAX 1024
static uint32_t s_extra_set[EXTRA_DRAWN_MAX + 16];

/** @brief Fill s_extra_set with the canonical drawn set followed by the glow owners not already in it.
 *
 * A glow ring's centre reads its owner's m_Position through the record's
 * MobyPos pointer, so an owner the array walk does not reach (the array
 * g_LevelMobys points at holds a copy of it) would draw the ring at the
 * tick's position under a sub-tick camera and body.
 * @return The number of entries. */
static int moby_glow_extra_set(void) {
    const uint32_t* drawn = NULL;
    int n = moby_native_canon_drawn_mobys(&drawn);
    if (n > EXTRA_DRAWN_MAX) n = EXTRA_DRAWN_MAX;
    if (n > 0) memcpy(s_extra_set, drawn, (size_t)n * sizeof *drawn);
    uint32_t owners[16];
    const int on = particle_glow_owner_mobys(owners);
    for (int k = 0; k < on; ++k) {
        int dup = 0;
        for (int i = 0; i < n && !dup; ++i) dup = s_extra_set[i] == owners[k];
        if (!dup) s_extra_set[n++] = owners[k];
    }
    return n;
}

/** @brief Sub-tick bracket for the dragon-rescue star record D_DRAGONSTAR.
 *
 * The record is 0x2C bytes: +0x00 gate, +0x04 world position, +0x10 the draw's
 * spoke matrix, +0x24 spoke size, +0x28 euler bytes Z/X/Y, +0x2B colour;
 * func_8002F3E4 animates it once per tick and the star feeder re-reads it on
 * every present. Stages the record through the view with position, size and
 * colour lerped prev->curr at the record's own alpha, and hands the feeder the
 * euler bytes lerped along the shortest arc at float precision; the feeder
 * composes them through the resolved camera matrix, the product gamestate 8
 * builds with RotVec8ToMatrix (func_8001CFDC), so at alpha 1 the matrix is the
 * canonical present's. The staged +0x10 is left as captured and is unread.
 * Snaps (no staging, no override) when either snapshot is inactive (a
 * re-trigger), on a move over 5000 world units, or on a turn over 64 byte units
 * on any axis. Guest memory is never written: a refused stage leaves the record
 * canonical. */
static void dragonstar_bracket(void) {
    const uint8_t* live = (const uint8_t*)openpete_psx_to_host_span(D_DRAGONSTAR, 0x2Cu);
    uint32_t gate = 0;
    if (live) memcpy(&gate, live, 4);
    const openpete_interp_obj_t* o = (gate != 0) ? openpete_interp_objs_find(D_DRAGONSTAR) : NULL;
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return;
    uint32_t pg, cg; memcpy(&pg, o->prev, 4); memcpy(&cg, o->curr, 4);
    int32_t pp[3], cp[3];
    memcpy(pp, o->prev + 0x04, 12); memcpy(cp, o->curr + 0x04, 12);
    double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
    double dang[3];
    int turn = 0;
    for (int k = 0; k < 3; k++) {
        dang[k] = (double)((((int)o->curr[0x28 + k] - (int)o->prev[0x28 + k] + 128) & 0xFF) - 128);
        turn |= (fabs(dang[k]) > 64.0);
    }
    if (pg == 0 || cg == 0 || turn || dx*dx + dy*dy + dz*dz > 5000.0*5000.0) return;
    uint8_t* st = (uint8_t*)openpete_subtick_view_stage(D_DRAGONSTAR, 0x2Cu);
    if (!st) return;
    double a2 = (double)(openpete_interp_present_now() - o->curr_ns) / interp_period_ns(o->prev_ns, o->curr_ns);
    if (a2 < 0.0) a2 = 0.0; else if (a2 > 1.0) a2 = 1.0;
    for (int k = 0; k < 3; k++) {
        int32_t v = pp[k] + (int32_t)llround(((double)cp[k] - pp[k]) * a2);
        memcpy(st + 0x04 + 4*k, &v, 4);
    }
    int32_t psz, csz; memcpy(&psz, o->prev + 0x24, 4); memcpy(&csz, o->curr + 0x24, 4);
    int32_t lsz = psz + (int32_t)llround(((double)csz - psz) * a2);
    memcpy(st + 0x24, &lsz, 4);
    int pc = o->prev[0x2B], ccol = o->curr[0x2B];
    st[0x2B] = (uint8_t)(pc + (int)llround(((double)ccol - pc) * a2));
    double ang[3];
    for (int k = 0; k < 3; k++) ang[k] = (double)o->prev[0x28 + k] + dang[k] * a2;
    dragonstar_native_set_rot_interp(ang);
}

/** @brief Slot `i` of the g_Particles pool as the led pool holds it, under --runahead.
 *
 * `pa` is the slot's guest address and `sel` its selector. Stages a captured
 * slot's bytes, hides a slot the led capture lacks, and ends the walk at the
 * led extent once the committed end sentinel has passed (`*past_end`).
 * @return 1 with *o and *ph set when the slot shows, 0 to skip it, -1 to end
 *         the walk */
static int particle_led_slot(uint32_t i, uint32_t pa, uint8_t* gh, uint8_t sel,
                             uint32_t led_extent, int* past_end,
                             const openpete_interp_obj_t** o, uint8_t** ph) {
    if (sel == 0xFFu) *past_end = 1;
    if (i >= led_extent) {
        if (*past_end) {                     /* the led pool ends here */
            if (sel != 0xFFu) particle_stage_sel(pa, 0xFFu);
            return -1;
        }
        if (!(sel & 0x80u)) particle_stage_sel(pa, PART_SEL_HIDDEN);
        return 0;
    }
    *o = openpete_interp_objs_find(pa);
    if (!*o) {                               /* inactive in the led pool */
        if (*past_end || !(sel & 0x80u)) particle_stage_sel(pa, PART_SEL_HIDDEN);
        return 0;
    }
    *ph = particle_stage_shown(pa, gh, *o);
    return 1;
}

/** @brief Bracket one shown g_Particles slot's position between its snapshot pair at its own alpha.
 *
 * The bracket sits behind the identity (class/sel, life) and teleport guards; a
 * guarded slot snaps to its shown bytes. Stages the whole slot and records the
 * guest position words in *e for the restore and the integrity check; `pi`
 * takes the verdict census.
 * @return 1 when *e was written, 0 when the slot snapped */
static int particle_slot_lerp(uint32_t pa, const uint8_t* gh, uint8_t* ph,
                              const openpete_interp_obj_t* o, op_time_t now,
                              long pi[8], part_save_t* e) {
    if (!o || !o->have_prev) { pi[1]++; return 0; }  /* spawn -> snap (curr) */
    op_time_t period = o->curr_ns - o->prev_ns;
    if (o->prev_ns == 0 || period <= 0) return 0;
    double pa_alpha = (double)(now - o->curr_ns) / interp_period_ns(o->prev_ns, o->curr_ns);
    if (pa_alpha < 0.0) pa_alpha = 0.0; else if (pa_alpha > 1.0) pa_alpha = 1.0;
    uint32_t pw1, pw2, cw1, cw2;
    memcpy(&pw1, o->prev + PART_POS_OFF, 4); memcpy(&pw2, o->prev + 0x08u, 4);
    memcpy(&cw1, o->curr + PART_POS_OFF, 4); memcpy(&cw2, o->curr + 0x08u, 4);
    int pX = (int16_t)(pw1 & 0xFFFF), pY = (int16_t)(pw1 >> 16), pZ = (int16_t)(pw2 & 0xFFFF);
    int cX = (int16_t)(cw1 & 0xFFFF), cY = (int16_t)(cw1 >> 16), cZ = (int16_t)(cw2 & 0xFFFF);
    /* Positions are world/4 (int16); the identity and teleport guards
     * below are in world/4 too. */
    double dX = cX - pX, dY = cY - pY, dZ = cZ - pZ;
    /* IDENTITY guard (slot reuse). The interp-objs table is keyed by slot
     * ADDRESS, so a slot a NEW particle respawns into within one tick - a
     * shatter/burst churning the pool faster than slots free - still reads
     * have_prev=1 and gets lerped between TWO DIFFERENT particles: a long
     * line streaks from the old position to the new one. The distance guard
     * below only catches FAR reuses; a burst clustered near its emitter
     * reuses well within it. Detect identity from the particle header word:
     * m_Class (byte0) + sel/type (byte1) is the stable type, and m_Life
     * (byte2) is an age counter that increments each tick for a continuing
     * particle and restarts when the slot is reused. A type change or a life
     * decrease snaps (present curr, no streak); equal life is lerped, see
     * below. */
    {
        int moving = (dX*dX + dY*dY + dZ*dZ > 4.0);
        uint32_t ph2, ch2; memcpy(&ph2, o->prev, 4); memcpy(&ch2, o->curr, 4);
        if ((ph2 & 0xFFFFu) != (ch2 & 0xFFFFu)) { pi[2]++; if (moving) pi[6]++; return 0; } /* m_Class/sel changed -> reuse */
        uint8_t plife = (uint8_t)(ph2 >> 16), clife = (uint8_t)(ch2 >> 16);
        /* Life DECREASED -> reset -> slot reuse: snap. Life EQUAL is
         * ambiguous: a burst reuse with the same life, or a persistent
         * particle whose life byte stalls (the circling damage stars).
         * Snapping the latter steps its orbit at the tick rate, so equal
         * life is lerped under the 300 world/4 per tick ceiling below:
         * orbiting stars move ~50-100 per tick, burst reuses jump farther
         * or reset life. */
        if (clife < plife) { pi[3]++; if (moving) pi[7]++; return 0; }
        pi[0]++; if (moving) pi[5]++;
    }
    /* Teleport/reuse ceiling at particle-physics scale:
     * under runahead the pair can span two prediction BRANCHES,
     * whose pool churn differs - slot reuse across branches can
     * pass the class + life-increase tests (no same-timeline
     * ordering to reset the life byte) and lerp as a screen-
     * crossing streak. 300 world/4 per tick == 1200 full-world
     * == ~20x Spyro's run speed: a real particle faster than
     * that crosses the screen in a couple of ticks, so snapping
     * it loses nothing visible. */
    if (dX*dX + dY*dY + dZ*dZ > 300.0 * 300.0) { pi[4]++; return 0; }
    uint32_t live1, live2;
    memcpy(&live1, gh + PART_POS_OFF, 4); memcpy(&live2, gh + 0x08u, 4);
    e->addr = pa;
    e->w1 = live1;
    e->w2 = live2;
    {   /* Stage the WHOLE record, not just the two written words: a
         * reader resolving any field of this particle must land in
         * the staged copy, or it would mix staged position with
         * canonical everything-else. */
        uint8_t* st = (uint8_t*)openpete_subtick_view_stage(pa, PART_STRIDE);
        e->staged = (st != NULL);
        if (st) ph = st;
    }
    int16_t lX = lerp16((int16_t)pX, (int16_t)cX, pa_alpha);
    int16_t lY = lerp16((int16_t)pY, (int16_t)cY, pa_alpha);
    int16_t lZ = lerp16((int16_t)pZ, (int16_t)cZ, pa_alpha);
    uint32_t nw1 = (uint32_t)(uint16_t)lX | ((uint32_t)(uint16_t)lY << 16);
    uint32_t shown2; memcpy(&shown2, ph + 0x08u, 4);
    uint32_t nw2 = (shown2 & 0xFFFF0000u) | (uint32_t)(uint16_t)lZ;
    memcpy(ph + PART_POS_OFF, &nw1, 4);
    memcpy(ph + 0x08u, &nw2, 4);
    return 1;
}

/** @brief Bracket the position of every shown g_Particles slot for a sub-tick present.
 *
 * Each active slot's position is lerped between its snapshot pair at the
 * slot's own alpha (the present instant against the pair's timestamps), so
 * smoke, triangle and line particles move at the present rate instead of
 * stepping at the tick rate. The pair is keyed by slot address in the
 * interp-objs table, the store the mobys use; a spawn has no previous snapshot
 * and snaps, and particle_slot_lerp's identity and teleport guards snap a
 * reused slot. The lerp reads only the two observed states and never runs the
 * particle update's rand(), so the rand-spawned cinders are covered. The
 * g_Sparkles pool (twinkle phase lerped in sparkle_build) and the glow pool
 * (centre on the owner moby, channels lerped in glow_build) get no position
 * bracket here; under --runahead both are staged as the led capture holds
 * them, and the g_Particles walk reproduces the led pool (particle_led_slot).
 * Records every slot whose position was bracketed in `save` (at most
 * PART_SAVE_MAX) for the restore and the integrity check.
 * @param save receives the bracketed slots
 * @return the number of entries written to `save` */
static int particle_pools_bracket(part_save_t* save) {
    int n = 0;
    /* g_Sparkles (live while m_Life, the low byte of +0x0C, is nonzero) and the
     * glow pool (live while the ring count at +0x00 is positive) as the led
     * capture holds them; their feeders lerp phase channels from the snapshot
     * pair and read everything else from the staged slot. */
    if (openpete_runahead_get() > 0) {
        stage_led_fixed_pool(G_SPARKLES, 8u, 0x18u, 0x0Cu, 0xFFu);
        stage_led_fixed_pool(D_GLOWS, 16u, 0x24u, 0x00u, 0xFFFFFFFFu);
    }

    op_time_t now = openpete_interp_present_now();
    /* OPENPETE_PART_INTERP_DBG: per-second census of pool-interp verdicts -
     * lerped vs each snap reason, MOVING particles (prev->curr delta > 2
     * world/4) counted separately. A moving particle that keeps snapping is
     * a 30 Hz stepper (the circling-stars class). */
    static int s_pidbg = -1;
    if (s_pidbg < 0) { const char* e = getenv("OPENPETE_PART_INTERP_DBG"); s_pidbg = (e && *e && *e != '0'); }
    static long s_pi[8]; /* 0 lerp,1 noprev,2 classchg,3 lifestall,4 far,5 lerp-moving,6 classchg-moving,7 lifestall-moving */
    uint8_t* pbase = (uint8_t*)openpete_psx_to_host_span(G_PARTICLES, 4u);
    uint32_t ppool = 0; if (pbase) memcpy(&ppool, pbase, 4);
    /* Under --runahead the walk reproduces the led pool the canonical
     * present composed: a slot the led capture holds shows its captured
     * bytes, a slot active only in the committed guest RAM is hidden, and
     * the end sentinel moves to the led extent when the led pool reached
     * past the committed one. */
    const int led = openpete_runahead_get() > 0;
    const uint32_t led_extent = (led && ppool >= 0x80000000u) ? particle_led_extent(ppool) : 0u;
    int past_end = 0;
    if (ppool >= 0x80000000u) {
        for (uint32_t i = 0; i < 320u && n < PART_SAVE_MAX; i++) {
            uint32_t pa = ppool + i * PART_STRIDE;
            uint8_t* gh = (uint8_t*)openpete_psx_to_host_span(pa, PART_STRIDE);
            if (!gh) break;
            uint8_t* ph = gh;
            uint32_t hdr; memcpy(&hdr, ph, 4);
            uint8_t sel = (uint8_t)((hdr >> PART_HDR_SEL) & 0xFFu);
            const openpete_interp_obj_t* o = NULL;
            if (led) {
                int v = particle_led_slot(i, pa, gh, sel, led_extent, &past_end, &o, &ph);
                if (v < 0) break;
                if (v == 0) continue;
            } else {
                if (sel == 0xFFu) break;            /* end of pool */
                if (sel & 0x80u) continue;          /* inactive slot */
                o = openpete_interp_objs_find(pa);
            }
            if (particle_slot_lerp(pa, gh, ph, o, now, s_pi, &save[n])) n++;
        }
    }
    if (s_pidbg) {
        static op_time_t t0 = 0;
        if (t0 == 0) t0 = now;
        if (now - t0 >= OP_NS_PER_SEC) {
            fprintf(stderr, "[part-interp f=%u] lerp=%ld (moving=%ld) noprev=%ld classchg=%ld (mov=%ld) lifestall=%ld (mov=%ld) far=%ld\n",
                    openpete_frame, s_pi[0], s_pi[5], s_pi[1], s_pi[2], s_pi[6], s_pi[3], s_pi[7], s_pi[4]);
            memset(s_pi, 0, sizeof s_pi);
            t0 = now;
        }
    }
    return n;
}

/** @brief Undo particle_pools_bracket: write each bracketed slot's saved position words back to guest RAM.
 *
 * A slot staged through the sub-tick view never touched guest RAM and is
 * skipped.
 * @param save The slots particle_pools_bracket recorded.
 * @param n The number of entries in save. */
static void particle_pools_restore(const part_save_t* save, int n) {
    for (int i = 0; i < n; i++) {
        if (save[i].staged) continue;   /* never written; nothing to undo */
        uint8_t* ph = (uint8_t*)openpete_psx_to_host_span(save[i].addr, PART_STRIDE);
        if (!ph) continue;
        memcpy(ph + PART_POS_OFF, &save[i].w1, 4);
        memcpy(ph + 0x08u,        &save[i].w2, 4);
    }
}

/** @brief Report every bracketed slot whose position words in guest RAM differ from the saved canonical words.
 *
 * Run after particle_pools_restore; one stderr line per mismatching slot.
 * @param save The slots particle_pools_bracket recorded.
 * @param n The number of entries in save. */
static void particle_pools_check(const part_save_t* save, int n) {
    for (int i = 0; i < n; ++i) {
        const uint8_t* ph = (const uint8_t*)openpete_psx_to_host_span(save[i].addr, PART_STRIDE);
        if (!ph) continue;
        uint32_t w1, w2;
        memcpy(&w1, ph + PART_POS_OFF, 4);
        memcpy(&w2, ph + 0x08u, 4);
        if (w1 != save[i].w1 || w2 != save[i].w2)
            fprintf(stderr, "[bracket-check f=%u] RESTORE MISMATCH particle@%08x\n",
                    openpete_frame, save[i].addr);
    }
}

/** @brief The sub-tick present body.
 *
 * Always entered through the wrapper game_reextract_view_install registers
 * (renderer/reextract_view.c), which owns the view's begin/end pairing: an
 * unpaired view would leave staged bytes visible to the next tick. */
static void game_reextract_body(void* cpu_v) {
    OP_ROLE_ASSERT(sim);
    CPUState* cpu = (CPUState*)cpu_v;
    game_interp_hud_batch_begin();   /* HUD pool keys: one table for this present's lookups */

    /* Bracket-integrity: snapshot the fixed DIRECT gameplay regions BEFORE
     * any lerp write, so they can be re-checked after all restores at the end of
     * this present. Bracketed spans = exactly what the feeders write below. */
    const int bc_on = bracket_check_enabled();
    static const struct { uint32_t addr, len; const char* name; } bc_regs[] = {
        { G_CAMERA, CAM_BYTES,   "g_Camera"      },
        { G_SPYRO,  SPYRO_SAVE,  "g_Spyro"       },
        { G_SPYROFLAME,  FLAME_SAVE,  "g_SpyroFlame"  },
        { D_SPYROSHADOW, SHADOW_SAVE, "g_SpyroShadow" },
        { D_DRAGONSTAR,  0x2Cu,       "d_DragonStar"  },
    };
    const int bc_n = (int)(sizeof(bc_regs) / sizeof(bc_regs[0]));
    uint64_t bc_h0[5];
    if (bc_on) for (int i = 0; i < bc_n; ++i) bc_h0[i] = bc_hash(bc_regs[i].addr, bc_regs[i].len);

    /* Pair prev/curr camera snapshots (region 0 = g_Camera). Returns 0 when
     * the world is stuck (byte-identical: paused / loading / vbl busy-wait)
     * or no pairing yet (spawn) - then we present the live (curr) scene with
     * no camera bracket. */
    double qa = 0.0;
    const openpete_interp_snapshot_t *prev2 = NULL, *prev = NULL, *curr = NULL;
    /* The query's own alpha (qa) is unused - the bracket alpha is derived
     * below from the present clock - but pass that clock here too, so the
     * re-extract reads one clock throughout. */
    int paired = openpete_interp_query3(openpete_interp_present_now(), TICK_PERIOD_NS, &qa, &prev2, &prev, &curr);
    /* Unpaired means there is no bracket to interpolate within (spawn, a
     * stuck world): the scene is presented live, and the pacing log is told
     * so rather than being given a stale alpha. */
    if (!paired) openpete_pace_note_alpha(0.0, 0);

    /* Rebuild the fractional interpolated-position table for this present (the
     * lerp32 1-unit floor fix). Empty until a bracket below populates it; feeders
     * fall back to reading the (floored) RAM for any key with no entry. Active for
     * the duration of this present only (cleared at the end) so the canonical
     * render path never reads a stale table. */
    fpos_reset();
    s_fpos_active = 1;

    /* Bracket targets: each is published through the view, or written in guest
     * RAM and restored on the host fiber when staging is refused. */
    /* The camera's sub-tick lerp publishes through the view: writes land in a
     * staged copy that every read during this present resolves to, so guest RAM
     * is never touched and there is nothing to restore. If staging is
     * unavailable the legacy write-and-restore against live RAM still runs --
     * dropping the interpolation instead would judder the camera, which is a
     * worse failure than the one being retired. */
    uint8_t* cam = (uint8_t*)openpete_subtick_view_stage(G_CAMERA, CAM_BYTES);
    const int cam_staged = (cam != NULL);
    if (!cam) cam = (uint8_t*)openpete_psx_to_host_span(G_CAMERA, CAM_BYTES);
    s_body_cam = cam;
    uint8_t  cam_saved[CAM_BYTES];
    int      cam_bracketed = 0;
    uint8_t* sp = NULL; uint8_t sp_saved[SPYRO_SAVE]; int sp_bracketed = 0;
    int sp_staged = 0;   /* sp points at the view's staged copy, not guest RAM */
    /* player_native_set_rot_interp reads PLAYER_PARTS_MAX gates and angle
     * triples unconditionally, so both arrays carry that many entries even
     * though this game fills only the three segments below; the trailing
     * entries stay zeroed and their parts stay inactive. */
    int      sp_rot_active[PLAYER_PARTS_MAX] = {0};   /* per-segment (body/head/tail) float-rot gate */
    double   sp_rot_ang[PLAYER_PARTS_MAX][3] = {{0}}; /* [seg][Z,X,Y] sub-tick float byte-angles */
    uint8_t* fl = NULL; uint8_t fl_saved[FLAME_SAVE]; int fl_bracketed = 0;
    int fl_staged = 0;
    double   fl_subframe = 0.0;  /* fractional counter advance -> flame sub-keyframe slide */
    uint8_t* sh = NULL; uint8_t sh_saved[SHADOW_SAVE]; int sh_bracketed = 0;
    int sh_staged = 0;
    static moby_save_t s_moby_save[MOBY_SAVE_MAX];
    int      moby_save_n = 0;
    static part_save_t s_part_save[PART_SAVE_MAX];
    int      part_save_n = 0;
    double   alpha = 0.0;

    /* Discontinuity guard: if the camera jumped, turned or broke its motion
     * this tick (level transition, cutscene shot change, teleport),
     * interpolating across it produces a visible swoop; cut_camera_reason
     * decides from the camera's steps over the three ring snapshots. */
    /* How old the forward endpoint is, in ticks of present time. Normally under
     * one - the ring captures every tick - but the sim can stop producing new
     * states while presents continue: a paused update, a load, the debug freecam
     * (which freezes UpdateSpyro). The pair is then arbitrarily old and still
     * the only one there is. */
    double pair_age = 0.0;
    if (curr) pair_age = (double)((int64_t)openpete_interp_present_now()
                                  - (int64_t)curr->tick_ns) / (double)TICK_PERIOD_NS;
    const int pair_stale = (pair_age > 4.0);

    const int cam_cut = (paired && cam && prev && curr)
                      ? camera_cut(prev2, prev, curr, pair_age) : 0;
    /* Moby occlusion-cull steering: suspend culling around camera cuts (the
     * few-presents-old occlusion grid is wrong everywhere across a cut) and
     * feed the measured per-tick pan speed (in screen px) so the feeder's
     * cached rects inflate with real camera motion. Unpaired presents (spawn /
     * world-stuck) get a short suspension - the camera may have moved
     * arbitrarily relative to the grid capture. */
    /* A cut the ring is still replaying long after the fact is not a reason to
     * suspend. Identical captures are deduped, so once the sim stops producing
     * new states the ring serves the last two DISTINCT ones on every present -
     * and if the camera jumped just before it stopped, that pair straddles the
     * jump forever. Interpolation must still refuse it (the pair is a real
     * discontinuity), but the grid has been rebuilt from the shown camera on
     * every present since, so culling has nothing to fear. Suspending there
     * switched moby occlusion culling off for the whole of any static view. */
    if (cam_cut && !pair_stale) openpete_native_occl_suspend(12);
    else if (cam_cut) moby_native_set_occl_pan(0.0f);   /* the pan estimate is stale too */
    int use_herm = 0;   /* pred1 gate (set inside the bracket block; the record loop reads it) */
    if (paired && cam && prev && curr && !cam_cut) {
        /* Per-tick rotation = the max angle any ROW direction moved, from the
         * per-row NORMALIZED dot products. NOT the trace formula - the Spyro
         * camera matrix is rows-orthogonal but NOT orthonormal (the Y row
         * bakes a ~0.625 aspect scale), so acos((trace-1)/2) reads ~0.8 rad for IDENTICAL
         * matrices and would inflate every rect off the grid. */
        const int16_t* pm = (const int16_t*)(prev->data + prev->region_offsets[CAM_REGION()] + CAM_OFF_PROJ);
        const int16_t* cm = (const int16_t*)(cam + CAM_OFF_PROJ);
        double worst = 1.0;
        for (int rw = 0; rw < 3; rw++) {
            double dp = 0, np = 0, nc = 0;
            for (int k = 0; k < 3; k++) {
                double a = pm[rw*3+k], b = cm[rw*3+k];
                dp += a * b; np += a * a; nc += b * b;
            }
            double denom = sqrt(np * nc);
            double c = denom > 0.0 ? dp / denom : 1.0;
            if (c < worst) worst = c;
        }
        if (worst > 1.0) worst = 1.0; else if (worst < -1.0) worst = -1.0;
        moby_native_set_occl_pan((float)(acos(worst) * 400.0)); /* rad/tick -> px/tick at the canonical H (≈288 + margin); the consumer scales it by the view H scale */
    } else if (!cam_cut) {
        openpete_native_occl_suspend(2);
    }

    /* One cut rule. The interval shows tick N-1 at the instant tick N completes
     * and reaches N at alpha 1, so a cut is shown at alpha 1 like every other
     * channel's step, not from alpha 0: the bracket runs with alpha pinned to 0
     * (the lerp at 0 is prev byte-for-byte) and the cut lands on the alpha-1
     * present. A pair the ring is still replaying long after the fact
     * (pair_stale) has no open interval to hold and shows the head. */
    const int cam_hold = cam_cut && !pair_stale &&
                         cut_camera_hold(interp_period_ns(prev->tick_ns, curr->tick_ns),
                                         prev, curr);
    const int cam_snap = cam_cut && !cam_hold;
    cut_set_camera_snap(cam_snap);

    if (paired && cam && prev && curr && (!cam_cut || cam_hold)) {
        /* 1-tick-lag alpha: at the instant tick N completed (now≈curr.tick_ns)
         * show prev (N-1); advance to curr (N) over the next sim period. The
         * forward endpoint is the KNOWN canonical N -> no extrapolation. */
        double period = interp_period_ns(prev->tick_ns, curr->tick_ns);
        alpha = (period > 0.0) ? (double)(openpete_interp_present_now() - curr->tick_ns) / period : 1.0;
        /* OPENPETE_ALPHA_MAP_DBG: raw present-time -> interp-time mapping, one
         * line per present BEFORE the clamp (judder forensics: shown time is
         * curr_tick + alpha − 1; discontinuities here ARE the on-screen judder). */
        {
            static int s_amdbg = -1;
            if (s_amdbg < 0) { const char* e = getenv("OPENPETE_ALPHA_MAP_DBG"); s_amdbg = (e && *e && *e != '0'); }
            if (s_amdbg) {
                extern int g_openpete_canon_lag_leg;
                fprintf(stderr, "[alpha-map f=%u leg=%s] now=%.3fms raw=%.3fms curr_ns=%.3fms period=%.3fms alpha_raw=%.3f tick=%u\n",
                        openpete_frame, g_openpete_canon_lag_leg ? "CANON" : "inter",
                        (double)openpete_interp_present_now() / 1e6, (double)op_now() / 1e6, (double)curr->tick_ns / 1e6, /* clock-guard: wall */ /* prints both */
                        period / 1e6, alpha, curr->tick);
            }
        }
        /* Allow slightly NEGATIVE alpha instead of clamping at 0: the canonical
         * present fires a few ms before its tick's grid stamp (present latency
         * jitter vs the PLL-smoothed capture stamps), and clamping would snap
         * shown time forward ~0.5 VBL at every roll, delivering motion as
         * tick-rate jumps (OPENPETE_ALPHA_MAP_DBG prints the mapping).
         * lerp(prev,curr,α<0) is a tiny backward extrapolation of a known
         * velocity - continuous and visually correct; the cubic Hermite is NOT
         * safe below 0, so the pred1 gate below requires alpha >= 0. */
        /* The registered regions stopped changing longer ago than the pair's
         * own cadence: prev->curr describes a motion that has ENDED, and the
         * head is the state this frame shows. Wall-clock phase reaches the same
         * answer by itself (now runs away from curr->tick_ns and the clamp
         * below pins alpha at 1); a present phased onto curr->tick_ns does not,
         * and would re-lerp the middle of that last motion at any distance
         * behind the frame it is labelled with. */
        if (openpete_interp_curr_is_stale()) alpha = 1.0;
        if (alpha < -0.35) alpha = -0.35; else if (alpha > 1.0) alpha = 1.0;
        /* Held cut: every channel in this bracket shows prev for the whole
         * interval. A negative alpha would extrapolate backwards through the
         * cut, which is the motion that does not exist. */
        if (cam_hold) alpha = 0.0;
        /* Publish the alpha this present will actually show. It is the
         * position in ANIMATION time, which is what decides whether a present
         * carried new motion or repeated the previous frame - two presents at
         * one alpha are the same image however far apart they were submitted,
         * and no submit timestamp can tell those apart. Taken after the clamp
         * so it reports what was drawn, not what was requested. */
        openpete_pace_note_alpha(alpha, 1);

        /* pred1 gate: use the cubic Hermite (centered start-tangent) only when a
         * valid prev2 exists AND the prev2->prev step is itself continuous - a cut
         * on that earlier step would make the centered tangent overshoot. The
         * DIRECT-ring feeders (camera + Spyro) share this gate; mobys/anim clocks
         * stay linear for now. Same 1-tick latency either way. */
        const uint8_t* pcam2 = NULL;
        if (prev2 && hermite_enabled() && alpha >= 0.0) {
            pcam2 = prev2->data + prev2->region_offsets[CAM_REGION()];
            const int32_t* p2p = (const int32_t*)(pcam2 + CAM_OFF_POS);
            const int32_t* pp  = (const int32_t*)(prev->data + prev->region_offsets[CAM_REGION()] + CAM_OFF_POS);
            double dx = (double)pp[0]-p2p[0], dy = (double)pp[1]-p2p[1], dz = (double)pp[2]-p2p[2];
            use_herm = (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0);
            if (use_herm) {   /* rotation cut on prev2->prev (same test as the curr guard) */
                const int16_t* p2m = (const int16_t*)(pcam2 + CAM_OFF_PROJ);
                const int16_t* pm  = (const int16_t*)(prev->data + prev->region_offsets[CAM_REGION()] + CAM_OFF_PROJ);
                double frob = 0; for (int i = 0; i < 9; i++) frob += (double)p2m[i] * pm[i];
                if (frob < 4096.0 * 4096.0) use_herm = 0;
            }
        }

        /* (a) Camera: Q12 matrices + position (singleton, region 0).
         * FORWARD ENDPOINT = the RING's curr, not live RAM: identical when
         * runahead is off (curr was captured from this very RAM at the
         * tick), but under runahead live RAM is the restored REAL tick
         * while ring-curr is the LED tick - using live RAM collapsed the
         * pair to one tick (prev==curr) and the camera stepped at the tick
         * rate (every other feeder is led via the objs table; only the DIRECT
         * regions snap back).
         *
         * Under runahead the camera region is COMMITTED-timeline
         * (ra_commit recapture, interp_regions.c): the pair is
         * branch-free, so no input-edge rebase - it would only kink a
         * timeline that cannot mispredict. The rebase arm survives solely
         * for the OPENPETE_RA_CAM_COMMIT=0 (led-camera) A/B config. */
        const uint8_t* pcam = prev->data + prev->region_offsets[CAM_REGION()];
        const uint8_t* ccam = curr->data + curr->region_offsets[CAM_REGION()];
        int ra_rebased = 0;
        if (openpete_ra_input_edge && ra_rebase_on() && s_shown_cam_ok &&
            !openpete_interp_ra_commit_active()) {
            pcam = s_shown_cam;      /* glide from the shown state (see above) */
            use_herm = 0;            /* linear only - no tangent across branches */
            ra_rebased = 1;
        }
        if (!cam_staged) memcpy(cam_saved, cam, CAM_BYTES);  /* legacy path only */
        if (use_herm) {
            herm_matrix(cam, pcam2, pcam, ccam, CAM_OFF_PROJ, alpha);
            herm_matrix(cam, pcam2, pcam, ccam, CAM_OFF_VIEW, alpha);
            herm_vec3  (cam, pcam2, pcam, ccam, CAM_OFF_POS,  alpha);
            fpos_set_h(G_CAMERA,
                       (const int32_t*)(pcam2 + CAM_OFF_POS),
                       (const int32_t*)(pcam  + CAM_OFF_POS),
                       (const int32_t*)(ccam  + CAM_OFF_POS), alpha);
        } else {
            lerp_matrix(cam, pcam, ccam, CAM_OFF_PROJ, alpha);
            lerp_matrix(cam, pcam, ccam, CAM_OFF_VIEW, alpha);
            lerp_vec3  (cam, pcam, ccam, CAM_OFF_POS,  alpha);
            /* Fractional camera position (un-rounded) for the floor fix - same lerp,
             * double precision. */
            fpos_set(G_CAMERA,
                     (const int32_t*)(pcam + CAM_OFF_POS),
                     (const int32_t*)(ccam + CAM_OFF_POS), alpha);
        }
        cam_bracketed = 1;
        memcpy(s_shown_cam, cam, CAM_BYTES);      /* what the eye sees now */
        s_shown_cam_ok = 1;
        (void)ra_rebased;

        /* Per-present trace (OPENPETE_LAG_TRACE): the interpolated camera pos
         * presented THIS frame. Across the present sequence it must progress
         * monotonically - a sawtooth (jump forward then back) is the canonical-
         * vs-intermediate strobe (ghosting). */
        static int lt = -1;
        if (lt < 0) { const char* e = getenv("OPENPETE_LAG_TRACE"); lt = (e && *e && *e != '0'); }
        if (lt) {
            const int32_t* ip = (const int32_t*)(cam + CAM_OFF_POS);
            const int32_t* pp = (const int32_t*)(pcam + CAM_OFF_POS);
            const int32_t* cp = (const int32_t*)(ccam + CAM_OFF_POS);
            fprintf(stderr, "[lag-trace f=%u] a=%.3f camX=%d camZ=%d prevX=%d currX=%d\n",
                    openpete_frame, alpha, ip[0], ip[2], pp[0], cp[0]);
        }

        /* (b) Spyro position (singleton, region SPYRO_REGION). */
        /* Same shape as the camera above: publish the lerp into the view so
         * guest RAM is untouched, falling back to write-and-restore only if
         * staging is unavailable. */
        /* The whole region: the record loop below restages each record's
         * g_Spyro bytes into this same span (a covered address returns the
         * same copy; a longer request would not). */
        sp = (uint8_t*)openpete_subtick_view_stage(G_SPYRO, PLAYER_L.rec_len);
        sp_staged = (sp != NULL);
        if (!sp) sp = (uint8_t*)openpete_psx_to_host_span(G_SPYRO, PLAYER_L.rec_len);
        if (sp) {
            const uint8_t* psp = prev->data + prev->region_offsets[SPYRO_REGION()];
            const uint8_t* csp = curr->data + curr->region_offsets[SPYRO_REGION()];
            static uint8_t s_psp_rb[SPYRO_SAVE];
            if (openpete_ra_input_edge && ra_rebase_on() && s_shown_sp_ok &&
                !openpete_interp_ra_commit_active()) {
                /* Rebase POSITION only (the shown pos is mid-lerp, valid);
                 * anim clocks/rot keep the table pair. LED-CAMERA A/B
                 * CONFIG ONLY: under analog input the stick differs almost
                 * every tick, so the edge fires continuously and a
                 * standing rebase re-anchors the lerp at the shown pos
                 * every tick - motion bunches into the late presents and
                 * the negative-alpha presents extrapolate BACKWARD from
                 * the shown point (visible jitter). The plain table pair
                 * absorbs the few-unit branch error invisibly, like the
                 * (rebase-free, smooth) mobys. */
                memcpy(s_psp_rb, psp, SPYRO_SAVE);
                memcpy(s_psp_rb, s_shown_sp, 12);
                psp = s_psp_rb;
            }
            if (!sp_staged) memcpy(sp_saved, sp, SPYRO_SAVE);  /* legacy path only */
            /* pred1 for Spyro's m_Position, gated on the camera's prev2 continuity
             * PLUS a Spyro-specific position-step check (Spyro can warp while the
             * camera holds). Falls back to the linear lerp otherwise. */
            int sp_herm = 0;
            const uint8_t* psp2 = NULL;
            if (use_herm && prev2) {
                psp2 = prev2->data + prev2->region_offsets[SPYRO_REGION()];
                const int32_t* p2 = (const int32_t*)psp2;
                const int32_t* pp = (const int32_t*)psp;
                double dx=(double)pp[0]-p2[0], dy=(double)pp[1]-p2[1], dz=(double)pp[2]-p2[2];
                sp_herm = (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0);
            }
            if (sp_herm) {
                herm_vec3(sp, psp2, psp, csp, 0, alpha);   /* +0x00 m_Position */
                fpos_set_h(G_SPYRO, (const int32_t*)psp2,
                           (const int32_t*)psp, (const int32_t*)csp, alpha);
            } else {
                lerp_vec3(sp, psp, csp, 0, alpha);    /* +0x00 m_Position */
                /* Fractional Spyro position (un-rounded) for the floor fix. */
                fpos_set(G_SPYRO, (const int32_t*)psp, (const int32_t*)csp, alpha);
            }
            /* Per-segment (body/head/tail) animation clock: lerp each
             * m_FrameProgress (0x24/0x25/0x26) when its keyframe pair
             * (frameA 0x1E/0x20/0x22, frameB 0x1F/0x21/0x23) is unchanged -
             * else snap (no blend across a knot or a transition reseed). */
            static const int frA[3] = {0x1E,0x20,0x22}, frB[3] = {0x1F,0x21,0x23},
                             prog[3] = {0x24,0x25,0x26};
            for (int s = 0; s < 3; s++) {
                if (psp[frA[s]] == csp[frA[s]] && psp[frB[s]] == csp[frB[s]]) {
                    int pa = psp[prog[s]], ca = csp[prog[s]];
                    int la = (int)((double)pa + ((double)ca - pa) * alpha + 0.5);
                    if (la < 0) la = 0; else if (la > 255) la = 255;
                    sp[prog[s]] = (uint8_t)la;
                }
            }
            /* Body/head/tail rotation (packed 3×8-bit euler @ +0xC/+0x10/+0x14):
             * computed at FLOAT precision and handed to the Spyro euler composer
             * (player_native_set_rot_interp) - NOT written back to the byte euler,
             * which would re-quantize to the 256/circle (1.4°/step) grid the same
             * way the moby path did. Body (s=0) always interpolates; head/tail carry
             * a 0-sentinel ("use body") so interpolate only when both endpoints are
             * real - else leave that segment on the byte-exact path (snap). */
            for (int s = 0; s < 3; s++) {
                uint32_t off = 0x0Cu + (uint32_t)s * 4u;
                uint32_t pr, cr; memcpy(&pr, psp + off, 4); memcpy(&cr, csp + off, 4);
                if (s != 0 && (pr == 0 || cr == 0)) continue;   /* sticky / transition -> byte path */
                for (int k = 0; k < 3; k++) {
                    int pbk = (int)((pr >> (k*8)) & 0xFFu), cbk = (int)((cr >> (k*8)) & 0xFFu);
                    int d = cbk - pbk; if (d > 128) d -= 256; else if (d < -128) d += 256;
                    sp_rot_ang[s][k] = (double)pbk + (double)d * alpha;
                }
                sp_rot_active[s] = 1;
            }
            sp_bracketed = 1;
            memcpy(s_shown_sp, sp, 12);            /* shown m_Position */
            s_shown_sp_ok = 1;
            /* Spyro leg of OPENPETE_LAG_TRACE (see the camera leg above):
             * shown position + pair endpoints per present. */
            if (lt) {
                const int32_t* ip = (const int32_t*)sp;
                const int32_t* pp = (const int32_t*)psp;
                const int32_t* cp = (const int32_t*)csp;
                fprintf(stderr, "[lag-trace-sp f=%u] a=%.3f spX=%d spZ=%d prevX=%d currX=%d\n",
                        openpete_frame, alpha, ip[0], ip[2], pp[0], cp[0]);
            }
        }

        /* (c) Flame breath pose (singleton, region FLAME_REGION). The ribbon is
         * rigidly attached to Spyro's mouth/head: m_Position (origin) + the
         * flame-attach matrix both advance at the 30 Hz tick, so without this the
         * flame snaps to its tick-N pose while the (interpolated) Spyro it should
         * sit on moves smoothly - the 30 Hz look. Lerp both at the same alpha as
         * Spyro so the flame tracks him.
         *
         * The ribbon ANIMATION (not just the pose) is re-run rather than lerped:
         * the strip is `flameMatrix·keyframe[counter]` over a STATIC keyframe
         * template (D_FLAME_GEOM, byte-constant across ticks), indexed by
         * the per-strand counter FL_CNT which advances by g_DeltaTime (2..4) each
         * tick. Re-running flame_build_strip with a sub-tick-advanced counter walks
         * the template at the present rate -> genuinely high-FPS flame animation
         * (NOT a lerp of two shapes). An intermediate counter is a legitimate game
         * state - it occurs naturally at g_DeltaTime=1 - and flame_build_strip is
         * fully data-driven by it, so the intermediate ribbon is faithful.
         *
         * Teleport-guard the flame's first active tick (prev pose stale from
         * before activation): snap when the origin jumps far. */
        fl = (uint8_t*)openpete_subtick_view_stage(G_SPYROFLAME, FLAME_SAVE);
        fl_staged = (fl != NULL);
        if (!fl) fl = (uint8_t*)openpete_psx_to_host_span(G_SPYROFLAME, FLAME_SAVE);
        if (fl) {
            const uint8_t* pfl = prev->data + prev->region_offsets[FLAME_REGION()];
            const uint8_t* cfl = curr->data + curr->region_offsets[FLAME_REGION()];
            if (!fl_staged) memcpy(fl_saved, fl, FLAME_SAVE);  /* legacy path only */
            /* The ambient stage: the flame record loop restages each record's
             * bytes over it (flame_bracket per record) and restores it. */
            fl_bracketed = flame_bracket(fl, pfl, cfl, alpha, &fl_subframe);
        }

        /* (d) Spyro drop-shadow ground anchor (singleton, region SHADOW_REGION).
         * The canonical shadow render caches the anchor (m_Position @ +0x10) each
         * tick; it tracks Spyro's ground projection but lives in a scratch global
         * the spine doesn't otherwise touch, so without this the shadow steps at
         * 30 Hz under the (interpolated) Spyro. Lerp it at the same alpha as Spyro
         * so the shadow tracks him; teleport-guard a jump (level transition / warp
         * / a frame Spyro lifts off -> re-grounds). (Moby shadows need no bracket -
         * they re-derive from each moby's already-interpolated m_Position.) */
        sh = (uint8_t*)openpete_subtick_view_stage(D_SPYROSHADOW, SHADOW_SAVE);
        sh_staged = (sh != NULL);
        if (!sh) sh = (uint8_t*)openpete_psx_to_host_span(D_SPYROSHADOW, SHADOW_SAVE);
        if (sh) {
            const uint8_t* psh = prev->data + prev->region_offsets[SHADOW_REGION()];
            const uint8_t* csh = curr->data + curr->region_offsets[SHADOW_REGION()];
            const int32_t* pp = (const int32_t*)(psh + SHADOW_POS_OFF);
            const int32_t* cp = (const int32_t*)(csh + SHADOW_POS_OFF);
            double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
            if (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0) {
                if (!sh_staged) memcpy(sh_saved, sh, SHADOW_SAVE);  /* legacy path only */
                lerp_vec3(sh, psh, csh, SHADOW_POS_OFF, alpha);
                sh_bracketed = 1;
            }
        }

    }

    /* Per-moby interpolation (indirect table, keyed by Moby vaddr) - runs
     * UNCONDITIONALLY with a PER-OBJECT alpha, NOT gated on the camera bracket.
     * The DIRECT ring dedupes the WHOLE camera+Spyro snapshot, so when BOTH are
     * byte-static - the player idling in a homeworld while the dragons keep their
     * idle animation running - the ring reports paired=0, so a camera-gated moby
     * block would be skipped entirely and freeze every moby's animation/movement at
     * the tick rate (cutscene dragons step while the camera is still).
     * Each moby instead sweeps across ITS OWN distinct-state interval
     * (o->prev_ns..o->curr_ns) - identical to the camera alpha when both advance
     * every tick (both stamped from op_now() one VBL apart, so spatial coherence
     * with a moving camera is preserved), and keeps animating when the camera is
     * static. Same per-object-cadence pattern as the HUD icons below. */
    cut_obj_present_begin();
    {
        op_time_t now = openpete_interp_present_now();
        uint8_t* base_p = (uint8_t*)openpete_psx_to_host_span(G_LEVELMOBYS, 4u);
        uint32_t arr = 0; if (base_p) memcpy(&arr, base_p, 4);
        /* First pass of the walk: the mobys the canonical render drew from OUTSIDE
         * g_LevelMobys (the balloonist's relocated balloon - see
         * moby_native_canon_drawn_mobys). Entries inside the array's extent were
         * already handled by phase 0 and are skipped. */
        int drawn_n = moby_glow_extra_set(), drawn_i = 0;
        const uint32_t* drawn = s_extra_set;
        uint32_t arr_end = arr;    /* one past the last slot phase 0 walked */
        {
            uint32_t m = arr; int guard = 0;
            int phase = (arr >= 0x80000000u) ? 0 : 1;   /* no array -> drawn set only */
            while (guard++ < 4096 && moby_save_n < MOBY_SAVE_MAX) {
                uint8_t* mh;
                if (phase == 0) {
                    mh = (uint8_t*)openpete_psx_to_host_span(m, MOBY_STRIDE);
                    if (!mh || mh[MOBY_STATE] == 0xFF) {   /* unmapped / array sentinel */
                        arr_end = m; phase = 1; continue;
                    }
                } else {
                    if (drawn_i >= drawn_n) break;
                    m = drawn[drawn_i++];
                    if (m >= arr && m < arr_end) continue;   /* phase 0 already had it */
                    mh = (uint8_t*)openpete_psx_to_host_span(m, MOBY_STRIDE);
                    if (!mh) continue;
                }
                uint8_t st = mh[MOBY_STATE];
                if (st < 0x80) {                      /* alive */
                    const openpete_interp_obj_t* o =
                        openpete_interp_objs_find(phase ? game_interp_moby_key(m) : m);
                    if (o && o->have_prev) {
                        /* Identity guard (slot reuse). The interp-objs table is keyed
                         * by the moby slot address, so a slot another moby is allocated
                         * into within one tick still reads have_prev=1 and would lerp
                         * between two unrelated mobys, position and keyframe shape. A
                         * class change (m_Class +0x36 names the model) or a changed
                         * slot allocation serial (moby_pair_reborn) holds tick N-1's
                         * moby until alpha 1 (moby_cut_kind). */
                        uint16_t pcls, ccls;
                        memcpy(&pcls, o->prev + 0x36u, 2);
                        memcpy(&ccls, o->curr + 0x36u, 2);
                        const int hold_kind = moby_cut_kind(o, pcls, ccls);
                        if (hold_kind >= 0) {
                            moby_serial_census(m, o, 0.0, 1);
                            moby_cut_hold(m, o, now, hold_kind);
                            goto next_moby;
                        }

                        /* Per-object alpha from the moby's OWN distinct-roll
                         * timestamps, over a denominator capped at one tick
                         * (interp_period_ns - a stale prev after a static stretch
                         * must not stretch the resume); clamps to curr once it
                         * settles. */
                        op_time_t period_ns = o->curr_ns - o->prev_ns;
                        if (o->prev_ns != 0 && period_ns > 0) {
                            double ma = (double)(now - o->curr_ns) / interp_period_ns(o->prev_ns, o->curr_ns);
                            /* same negative-alpha allowance as the camera (roll-
                             * boundary continuity); hermite gated on ma >= 0 below */
                            if (ma < -0.35) ma = -0.35; else if (ma > 1.0) ma = 1.0;
                            ma = moby_held_cut_alpha(o, ma);
                            int32_t pp[3], cp[3];
                            memcpy(pp, o->prev + MOBY_POS, 12);
                            memcpy(cp, o->curr + MOBY_POS, 12);
                            /* per-moby teleport guard: a respawn / warp jumps far in
                             * one tick - lerping streaks it across the screen. The
                             * record holds tick N-1 whole until alpha 1 (moby_cut_hold). */
                            const int jumps = moby_step_jumps(m, o);
                            moby_serial_census(m, o, ma, jumps);
                            if (jumps) {
                                moby_cut_hold(m, o, now, CUT_TELEPORT);
                                goto next_moby;
                            }
                            s_moby_save[moby_save_n].addr = m;
                            memcpy(s_moby_save[moby_save_n].bytes, mh, MOBY_STRIDE);
                            {   /* publish this record's lerp through the view */
                                uint8_t* st = (uint8_t*)openpete_subtick_view_stage(
                                                  m, MOBY_STRIDE);
                                s_moby_save[moby_save_n].staged = (st != NULL);
                                if (st) mh = st;
                            }
                            moby_save_n++;
                            /* pred1: use the cubic Hermite (centered start-tangent)
                             * when this moby has a valid prev2 whose prev2->prev step
                             * is itself continuous - else the linear prev->curr lerp.
                             * Same per-object alpha + 1-tick latency either way. */
                            int m_herm = 0; int32_t p2[3]; int have_p2 = 0;
                            if (o->have_prev2) {
                                memcpy(p2, o->prev2 + MOBY_POS, 12);
                                have_p2 = !cut_obj_prev_jump(m, o->prev2, o->prev);
                                m_herm = hermite_enabled() && have_p2 && ma >= 0.0
                                /* Cutscene path actors: LINEAR position lerp only.
                                 * Their pose (two-pass shape lerp, linear in
                                 * balpha) carries baked whole-body motion authored
                                 * to complement the quantized path - the two
                                 * channels cancel per present ONLY when they
                                 * sweep with the SAME alpha shape; hermite easing
                                 * on position alone breaks the cancellation by up
                                 * to easing-gap × staircase amplitude. */
                                         && !spyro_cutpath_is_actor(m);
                            }
                            for (int k = 0; k < 3; k++) {
                                int32_t lv = m_herm ? hround32(hermite1(p2[k], pp[k], cp[k], ma))
                                                    : lerp32(pp[k], cp[k], ma);
                                memcpy(mh + MOBY_POS + k * 4, &lv, 4);
                            }
                            /* Fractional moby position (un-rounded) for the floor
                             * fix - keyed by the moby vaddr, at this moby's own
                             * per-object alpha. The body / shaded / shadow feeders
                             * look it up so the moby's own motion is continuous too
                             * (not just the camera component). */
                            if (m_herm) fpos_set_h(m, p2, pp, cp, ma);
                            else        fpos_set(m, pp, cp, ma);
                            /* OPENPETE_MOBY_POSTRACE_CLS=<cls>: per-present lerp
                             * trace for mobys of that class (jitter forensics). */
                            {
                                static long s_ptcls = -2;
                                if (s_ptcls == -2) { const char* e = getenv("OPENPETE_MOBY_POSTRACE_CLS"); s_ptcls = e ? strtol(e, NULL, 0) : -1; }
                                uint16_t tcls; memcpy(&tcls, mh + 0x36, 2);
                                if (s_ptcls >= 0 && (s_ptcls == 0xFFFF || tcls == (uint16_t)s_ptcls)) {
                                    int32_t lv2[3];
                                    memcpy(lv2, mh + MOBY_POS, 12);
                                    double cfp[3] = {0,0,0};
                                    int cfh = openpete_native_interp_pos(G_CAMERA, cfp);
                                    fprintf(stderr, "[moby-lerp f=%u] m=%08x herm=%d a=%.3f out=(%d,%d,%d) cam=%d(%.1f,%.1f,%.1f) rel=(%.1f,%.1f,%.1f)\n",
                                            openpete_frame, m, m_herm, ma,
                                            lv2[0], lv2[1], lv2[2],
                                            cfh, cfp[0], cfp[1], cfp[2],
                                            lv2[0]-cfp[0], lv2[1]-cfp[1], lv2[2]-cfp[2]);
                                }
                            }
                            /* Animation: NOT synthesized into RAM here. Writing an
                             * interpolated m_FrameProgress (or, across a knot, an
                             * extended "continuous-phase" blend) into mh[0x3E/0x3F/0x40]
                             * would make the moby decode sample a keyframe pair at a
                             * blend the game never rendered, stretching fast-animating
                             * mobys off-model. The moby decode instead does a
                             * TWO-PASS shape lerp: moby_decode_project
                             * reconstructs the prev REAL pose (moby_prev_keyframe reads
                             * this moby's o->prev frames) and the curr REAL pose and
                             * lerps them in model space at this same per-object alpha.
                             * So we leave mh's m_Frame/m_NextFrame/m_FrameProgress at the
                             * canonical curr values (the decode reads them as the curr
                             * pose) and let the feeder handle the interpolation. */
                            /* Rotation (m_Rotation @ +0x44, Vector3D8) is NOT lerped
                             * into RAM here: a byte write would re-quantize to the
                             * 1.4°/step grid (256/circle) and the sub-tick interp
                             * couldn't subdivide below it - the residual stair-step on
                             * slow rotators (gems/dragons). Instead the moby euler
                             * composer (shaded_compose_euler) reads this moby's
                             * prev/curr snapshots and renders the rotation at FLOAT
                             * precision (table-interpolated sin/cos). Enabled for the
                             * feeder pass via moby_native_set_rot_interp below. */
                        }
                    }
                }
            next_moby:
                if (phase == 0) m += MOBY_STRIDE;
            }
        }
    }

    /* Static 3D HUD icons (g_Hud.m_Mobys[12]) live OUTSIDE g_LevelMobys (so the
     * moby loop above misses them) and animate while the camera is dead still -
     * the inventory. So this runs UNCONDITIONALLY (not gated on the camera being
     * paired) with a PER-OBJECT alpha (the camera's alpha clamps to 1 when static,
     * which would step them). Two transitions to smooth: the m_Rotation (+0x44)
     * SPIN, and the m_Position (+0x0C) fly/slide-in when the HUD first appears.
     * Bracketed via s_moby_save (restored with the rest below). */
    {
        op_time_t now = openpete_interp_present_now();
        for (uint32_t i = 0; i < G_HUD_COUNT && moby_save_n < MOBY_SAVE_MAX; i++) {
            uint32_t m = G_HUD_ICONS + i * MOBY_STRIDE;
            uint8_t* mh = (uint8_t*)openpete_psx_to_host_span(m, MOBY_STRIDE);
            if (!mh) continue;
            const openpete_interp_obj_t* o = openpete_interp_objs_find(m);
            if (!o || !o->have_prev) continue;
            /* Per-object alpha from the icon's OWN distinct-roll timestamps:
             * sweeps 0->1 across its animation interval (independent of the camera,
             * which may be static), and clamps to curr once it stops moving
             * (curr_ns goes stale) so a settled icon doesn't wobble prev<->curr. */
            op_time_t period = o->curr_ns - o->prev_ns;
            if (o->prev_ns == 0 || period <= 0) continue;     /* no interval yet -> show curr */
            double ha = (double)(now - o->curr_ns) / interp_period_ns(o->prev_ns, o->curr_ns);
            if (ha < 0.0) ha = 0.0; else if (ha > 1.0) ha = 1.0;
            s_moby_save[moby_save_n].addr = m;
            memcpy(s_moby_save[moby_save_n].bytes, mh, MOBY_STRIDE);
            {
                uint8_t* st = (uint8_t*)openpete_subtick_view_stage(m, MOBY_STRIDE);
                s_moby_save[moby_save_n].staged = (st != NULL);
                if (st) mh = st;
            }
            moby_save_n++;
            /* Position (m_Position +0x0C): when the HUD appears the icons slide
             * down (Y, ~3-5 units/tick) into place - lerp that so the slide is as
             * smooth as the spin. Teleport-guard the one-tick appearance jump (the
             * initial placement is ~2880 units in Z/X) so it snaps into place
             * instead of streaking from a possibly-garbage spawn position. The
             * guard sits well above the slide (~5) and below the placement jump. */
            {
                int32_t pp[3], cp[3];
                memcpy(pp, o->prev + MOBY_POS, 12);
                memcpy(cp, o->curr + MOBY_POS, 12);
                double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
                if (dx*dx + dy*dy + dz*dz <= 512.0 * 512.0)
                    for (int k = 0; k < 3; k++) {
                        int32_t lv = lerp32(pp[k], cp[k], ha);
                        memcpy(mh + MOBY_POS + k * 4, &lv, 4);
                    }
            }
            /* Rotation (m_Rotation +0x44): NOT lerped here - the HUD icons route
             * through the moby euler composer (shaded_compose_euler), which now
             * renders rotation at float precision from their prev/curr interp-objs
             * snapshots (moby_native_set_rot_interp). A byte write here would
             * re-quantize to the 256/circle grid and be ignored by that float path. */
        }
    }

    /* g_Particles slots, and the dragon-star record (D_DRAGONSTAR) staged
     * through the view with the sub-tick position, size and colour, its
     * rotation reaching the feeder as float euler angles (dragonstar_bracket).
     * A held cut brackets neither: particle_reextract stages tick N-1's copies
     * of both whole. */
    if (!cam_hold) {
        part_save_n = particle_pools_bracket(s_part_save);
        dragonstar_bracket();
    }

    /* The 2D-UI overlay (fade + cutscene letterbox + menu panel + the 2D-UI
     * sprites/lines) is ACCUMULATED at feed time inside add_tri (so this rebuild's
     * scene wipe can't lose a mid-VBL feed) and committed to the carry set by
     * openpete_native_scene_capture_overlay() in run.c's CANONICAL present block.
     * The reextract only RE-APPLIES it (below), on every present. */

    /* Render camera lock (G): override the bracketed camera for THIS present's
     * extraction. Runs after every other bracket so nothing re-derives from the
     * interpolated pair we replace; the shared restore at the end of the present
     * puts the true bytes back before the sim resumes (bracket-integrity
     * checked), so the lock is render-only. The occlusion grid is keyed to the
     * TRUE camera's screen rects, so culling stays suspended while locked. */
    /* Scripted engage-at-frame (headless A/Bs): latch whatever view is live
     * at VBL N - engaging at boot would latch a mid-load camera. */
    {
        static long s_cl_frame = -2;
        if (s_cl_frame == -2) {
            const char* e = getenv("OPENPETE_CAMLOCK_FRAME");
            s_cl_frame = (e && *e) ? atol(e) : -1;
        }
        if (s_cl_frame >= 0 && !openpete_camlock_active() &&
            openpete_frame >= (unsigned)s_cl_frame) {
            s_cl_frame = -1;   /* one-shot */
            openpete_camlock_toggle();
        }
    }
    if (openpete_camlock_active() && cam) {
        if (!cam_bracketed) {
            if (!cam_staged) memcpy(cam_saved, cam, CAM_BYTES);
            cam_bracketed = 1;
        }
        cl_apply(cam);
        memcpy(s_shown_cam, cam, CAM_BYTES);
        s_shown_cam_ok = 1;
        openpete_native_occl_suspend(2);
    }

    /* Restore tick-N's transient .s render-pass scratch (cull records, colour,
     * shaded list, Spyro seg + SXY) so the read-only body feeders below find
     * valid inputs - without it they emit nothing and mobys/Spyro flicker. */
    scratch_bracket_enter(cpu, cam_hold);

    /* Sub-tick keyframe-blend for Spyro: pass the alpha + whether interpolation
     * is active (camera bracketed). The feeder builds a blend-interpolated seg
     * scratch so Spyro's animation is as smooth as its movement. */
    player_native_set_anim_interp(alpha, cam_bracketed);
    /* Spyro body/head/tail rotation at float precision (same fix as the mobys):
     * the per-segment float byte-angles computed above feed spyro_compose_euler so
     * Spyro's turn/sway renders below the 8-bit euler grid. Inactive segments
     * (head/tail sticky/transition, or not bracketed) fall back to byte-exact. */
    player_native_set_rot_interp(sp_bracketed ? sp_rot_active : NULL, &sp_rot_ang[0][0]);
    /* Flame sub-keyframe slide: the fractional part of the counter advance (the
     * integer part is already written to FL_CNT). The flame feeder lerps each
     * cross-section between adjacent spine records by this, so the ribbon animates
     * below the keyframe grid. 0 when not bracketed (exact keyframes). Set per
     * flame record by flame_records_present below; the ambient value is the
     * ring pair's, unused by any other feeder. */
    (void)fl_subframe;
    /* Moby rotation: render at float precision (table-interpolated sin/cos over the
     * per-moby prev/curr euler snapshots) for this feeder pass, so slow rotators
     * (gems, idle dragons, HUD icons) don't stair-step on the 1.4° byte grid.
     * Per-object self-gated inside the composer; reset after the feeders so the
     * canonical inline render stays byte-exact. */
    moby_native_set_rot_interp(1);

    /* Rebuild the scene against the (now interpolated) gamestate. scene_begin
     * resets s_scene and stamps it with openpete_frame, so the feeders' own
     * begin_frame() calls no-op (no history snapshot / no double-reset). */
    openpete_native_scene_begin();
    held_background_open(cpu, cam_hold);
    uint32_t bg = cyclorama_native_bg_word(cpu) & 0xFFFFFF;
    openpete_native_scene_set_bg(bg | 0xFF000000u);   /* fallback bg if cyclorama is off */

    /* Re-extract each feeder in canonical scene order. Camera-direct feeders
     * (terrain, Spyro) + the re-derived mobys interpolate via the brackets; sky
     * reproduces tick-N (far backdrop). Every feeder runs -> nothing vanishes. */
    const int pt_on = openpete_phase_timers_on();   /* per-feeder phase timing */
    uint64_t pt_t0 = 0, pt_t1;
    { OP_PROF_ACCUM_SCOPE("rex.transition");
      transition_reextract(cpu, cam_hold);     /* level-transition / balloonist cross-fade backdrop (backmost) */ }
    { OP_PROF_ACCUM_SCOPE("rex.cyclorama");
      cyclorama_reextract(cpu, alpha, cam_bracketed); /* sky dome + background (transition matrices lerped prev->curr) */ }
    { OP_PROF_ACCUM_SCOPE("rex.portal");
      portal_reextract(cpu, alpha, cam_bracketed, cam_hold); /* homeworld portal previews (dome matrix lerped prev->curr) */ }
    if (pt_on) pt_t0 = openpete_phase_now_us();
    { OP_PROF_ACCUM_SCOPE("rex.terrain");
      terrain_reextract(cpu, alpha, cam_bracketed, cam_hold); /* interpolated camera + poly-anim vert lerp */ }
    portal_reextract_leave();      /* terrain read the held set's opening planes */
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_REX_TERRAIN, pt_t1 - pt_t0); pt_t0 = pt_t1; }
    moby_native_set_verdict_alpha(alpha, cam_bracketed);
    moby_native_replay_prev_queue(cam_hold);   /* a record staged at tick N-1 shows that tick's state */
    { OP_PROF_ACCUM_SCOPE("rex.moby");
      moby_reextract(cpu);           /* re-derived: interpolated camera + lerped position */ }
    moby_native_replay_prev_queue(0);
    moby_native_set_verdict_alpha(1.0, 1);
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_REX_MOBY, pt_t1 - pt_t0); pt_t0 = pt_t1; }
    shadow_replay_prev_list(cam_hold);
    { OP_PROF_ACCUM_SCOPE("rex.shadow");
      shadow_reextract(cpu);         /* drop-shadows under mobys (Spyro's: the record loop below) */ }
    shadow_replay_prev_list(0);
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_REX_SHADOW, pt_t1 - pt_t0); }
    /* The moby + shadow feeders have now consumed the FULL regular cull records;
     * overlay the Spyro seg scratch over the (consumed) records 14+ at 0x1900 -
     * the .s's own temporal reuse - so the Spyro body feeder sees its seg scratch
     * without record 14+ mobys (Sparx, distant chests) being clobbered earlier. */
    scratch_bracket_seg_restore(cpu);
    if (pt_on) pt_t0 = openpete_phase_now_us();
    player_records_set_hold(cam_hold);   /* a held cut shows tick N-1's records whole */
    { OP_PROF_ACCUM_SCOPE("rex.player");
      player_records_loop(cpu, paired, cam_snap, cam_bracketed, alpha, use_herm,
                         sp, sp_staged, prev ? prev->data + prev->region_offsets[SPYRO_REGION()] : NULL,
                         sh); }
    player_records_set_hold(0);
    if (pt_on) { pt_t1 = openpete_phase_now_us(); openpete_phase_add(OPPT_REX_SPYRO, pt_t1 - pt_t0); }
    cyclorama_native_hold_background(0);
    { OP_PROF_ACCUM_SCOPE("rex.particles");
      particle_reextract(cpu, cam_hold);   /* particles / glows / sparkles (Sparx) */ }
    player_records_set_hold(cam_hold);   /* the flame records are the player's */
    { OP_PROF_ACCUM_SCOPE("rex.flame");
      flame_records_present(cpu, paired, cam_snap, alpha, fl);   /* fire breath, one ribbon per flame record */ }
    player_records_set_hold(0);
    { OP_PROF_ACCUM_SCOPE("rex.tracers");
      tracer_reextract(cpu);          /* additive tracer/beam (replay snapshot, interp camera) */ }
    moby_native_set_rot_interp(0);        /* reset: canonical inline render uses byte-exact rotation */
    player_native_set_rot_interp(NULL, NULL); /* reset: canonical inline render uses byte-exact rotation */
    player_native_set_anim_pair(NULL, NULL);  /* reset: the canonical hook owns the seg rows */
    player_native_set_player_slot(0);

    /* Re-apply the 2D-UI overlay (letterbox + menu panel held; fade interpolated
     * by the spine alpha) on top of the re-derived 3D scene. */
    { OP_PROF_ACCUM_SCOPE("rex.overlay");
      openpete_native_scene_apply_overlay(alpha); }

    /* The camera this present shows, whatever the guard decided: the bracket
     * writes it, a guard that snaps leaves the canonical tick-N bytes. One line
     * per intermediate present, so a sweep over alpha reads the camera step
     * against the point where the previous interval ended. */
    if (cam)
        cut_census_camera((const int32_t*)(cam + CAM_OFF_POS),
                          (const int16_t*)(cam + CAM_OFF_PROJ), alpha, cam_cut);

    /* Restore the transient render-pass scratch (disjoint from the position
     * bracket regions below - order irrelevant). */
    scratch_bracket_leave(cpu);

    /* Restore canonical RAM before the game fiber resumes (the present below
     * reads only s_scene). Order is irrelevant - the regions are disjoint. */
    for (int i = 0; i < moby_save_n; i++) {
        if (s_moby_save[i].staged) continue;   /* never written; nothing to undo */
        uint8_t* mh = (uint8_t*)openpete_psx_to_host_span(s_moby_save[i].addr, MOBY_STRIDE);
        if (mh) memcpy(mh, s_moby_save[i].bytes, MOBY_STRIDE);
    }
    particle_pools_restore(s_part_save, part_save_n);
    dragonstar_native_set_rot_interp(NULL);   /* canonical presents compose from the bytes */
    if (sh_bracketed && !sh_staged)  memcpy(sh, sh_saved, SHADOW_SAVE);
    if (fl_bracketed && !fl_staged)  memcpy(fl, fl_saved, FLAME_SAVE);
    if (sp_bracketed && !sp_staged)  memcpy(sp, sp_saved, SPYRO_SAVE);
    if (cam_bracketed && !cam_staged) memcpy(cam, cam_saved, CAM_BYTES);

    /* Bracket-integrity: every lerp write above must be undone - verify the
     * present left gameplay RAM byte-identical to how it found it. A mismatch is
     * a restore bug (the class the headless oracle can't see); log it same-frame.
     * The moby/particle pool checks guard their restore loops against future
     * regressions (a slot dropped from the loop, a stray post-restore write). */
    /* Load-window census (OPENPETE_LOAD_CENSUS=1): one line per re-extract with
     * the inputs that decide what the intermediates rebuild - the canon-drawn
     * mask (which paths the last canonical render actually painted), the
     * camera-ring pairing, and the cut flag. Correlate with the MOBY-RE and
     * SCENE lines to attribute "mobys visible during a level transition". */
    {
        static int s_census = -1;
        if (s_census < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); s_census = (e && *e && *e != '0'); }
        if (s_census) {
            uint32_t m = 0;
            for (int p = 0; p < RP_COUNT; p++) if (render_path_canon_drawn((render_path_t)p)) m |= 1u << p;
            fprintf(stderr, "[census f=%u RE] canon_mask=%02x (cyc|spy|moby|ter|shdw|flm|part|trc) paired=%d cam_cut=%d alpha=%.2f mobys=%d\n",
                    openpete_frame, m, paired, cam_cut, alpha, moby_save_n);
        }
    }
    if (bc_on) {
        for (int i = 0; i < bc_n; ++i) {
            if (bc_hash(bc_regs[i].addr, bc_regs[i].len) != bc_h0[i])
                fprintf(stderr, "[bracket-check f=%u] RESTORE MISMATCH region=%s: "
                        "sub-tick present perturbed gameplay RAM\n",
                        openpete_frame, bc_regs[i].name);
        }
        for (int i = 0; i < moby_save_n; ++i) {
            const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_span(s_moby_save[i].addr, MOBY_STRIDE);
            if (mh && memcmp(mh, s_moby_save[i].bytes, MOBY_STRIDE) != 0)
                fprintf(stderr, "[bracket-check f=%u] RESTORE MISMATCH moby@%08x\n",
                        openpete_frame, s_moby_save[i].addr);
        }
        particle_pools_check(s_part_save, part_save_n);
    }

    s_fpos_active = 0;   /* leave the resolver inert for the canonical render path */
    s_body_cam = NULL;
    cut_obj_present_end();

    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_REEXTRACT_DBG"); dbg = (e && *e && *e != '0'); }
    if (dbg) {
        static unsigned n = 0, last_frame = 0; static double amin = 2.0, amax = -1.0;
        if (cam_bracketed) { if (alpha < amin) amin = alpha; if (alpha > amax) amax = alpha; }
        n++;
        if (openpete_frame != last_frame && (openpete_frame % 120u) == 0) {
            last_frame = openpete_frame;
            fprintf(stderr, "[reextract f=%u] intermediates=%u paired=%d bracketed=%d mobys=%d "
                    "alpha=[%.2f..%.2f] captures=%llu dedupe=%llu\n",
                    openpete_frame, n, paired, cam_bracketed, moby_save_n,
                    amin <= 1.0 ? amin : -1.0, amax,
                    (unsigned long long)openpete_interp_total_captures(),
                    (unsigned long long)openpete_interp_total_dedupe_skips());
            amin = 2.0; amax = -1.0;
        }
    }
    game_interp_hud_batch_end();
}

/** @brief Install the sub-tick re-extract body with the native renderer (game_reextract_init). */
static void orch_reextract_init(void) {
    game_reextract_view_install(game_reextract_body);
}

/* The orchestrator spyro1_game_runtime carries (game_extras_impl.c). */
const game_orchestrator_t spyro1_orchestrator = {
    .reextract_init    = orch_reextract_init,
    .interp_pos        = orch_interp_pos,
    .epoch_commit      = orch_epoch_commit,
    .epoch_supersede   = orch_epoch_supersede,
    .epoch_stamp       = orch_epoch_stamp,
    .epoch_fresh       = orch_epoch_fresh,
    .mobyreg_fresh     = orch_mobyreg_fresh,
    .shaded_fresh      = orch_shaded_fresh,
    .capture_range     = orch_capture_range,
    .capture_recs      = orch_capture_recs,
    .capture_sxy       = orch_capture_sxy,
    .capture_camera    = orch_capture_camera,
    .prev_recs         = orch_prev_recs,
    .prod_camera_enter = orch_prod_camera_enter,
    .prod_camera_leave = orch_prod_camera_leave,
};
