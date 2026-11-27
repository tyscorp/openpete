// games/common/renderer/particles.c - the particle feeder for a game whose
// particle renderer links its primitives into the 3D ordering table itself.
//
// The routine (asm/partsdraw.s) is handwritten: it loads the camera matrix
// into the GTE with a zero translation, walks a pool of fixed-stride records
// and appends one primitive per drawn record to the polybuf. Six classes:
// a 4-word LINE_F2 dot, a 5-word LINE_G2 streak, and 10-word POLY_FT4
// billboards. Because it links each primitive itself, none of them reaches
// the 2D ordering-table chokepoint the sprite feeder hooks, so without this
// feeder the native scene shows no particles at all.
//
// The captured primitives supply each draw's code, colour, texture page, CLUT
// and UVs. The geometry of a paired primitive is rebuilt in float from its
// record's state (part_build), as the routine builds it in integers, at the
// capture and at each sub-tick present: the routine's screen positions are
// the GTE's int16 output, truncated in its MACs and its screen divide, so
// read back they sit up to several pixels off the record (near the camera)
// by an amount that changes every tick. The primitives carry no depth either
// - the PSX orders them by ordering-table bucket - so the feeder recovers
// each primitive's view-Z by projecting its own record's position, pairing
// primitives to records through the stamp byte the routine maintains
// (partsdraw.s:223 clears it when a record's processing starts, :358, :415,
// :528, :693 and :745 set it once the record passes its screen and depth
// window) and the per-class key test some classes apply after the stamp
// (game_layout.particles.stamp_skip), which makes the pairing the routine's
// own invariant rather than a match on geometry. Class `fixed_layer_class` and up link at a fixed front bucket
// (partsdraw.s:202), stamp nothing and append one primitive per record
// unconditionally, so they pair with their records by walk order and take the
// near depth that bucket means. Line numbers cite the
// Spyro 3 asm/partsdraw.s; the Spyro 2 routine is the same code two lines
// lower.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the stamp bytes and
// the polybuf cursor; this feeder only reads them afterwards. The pool's slot
// allocator (game_layout.owner.part_alloc) runs exactly once through the same
// kind of hook, which reads the slot it returned.
//
// Gated by OPENPETE_PARTICLE_CENSUS (census) or a native renderer mode
// (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_interp_objs.h"   /* the per-record prev/curr pool snapshots */
#include "openpete_subtick_view.h"  /* openpete_subtick_view_active: the trace's leg */
#include "openpete_fiber.h"         /* openpete_feed_key_now: the trace's window */
#include "renderer/particles.h"
#include "renderer/alloc_serial.h"   /* alloc_serial_next */
#include "renderer/orchestrator.h"  /* the canonical-render epoch */
#include "renderer/cut_guard.h"     /* cut_held_set_pick, cut_census_held_set */
#include "renderer/moby.h"          /* moby_native_canon_ref_ns */
#include "renderer/player_hide.h"   /* --hide-player: the player's records */
#include "no_lod.h"                 /* openpete_no_lod: extended LOD reveals records past the depth bound */
#include "openpete_recomp_module.h"   /* the allocator's own body */

#include "game_layout.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_pace_log.h"      /* OP_PACE_SPLIT_SCOPE: the guest call's split */
#include "openpete_profile.h"       /* the guest call's profiler region */
#include "openpete_thread_roles.h"

extern unsigned openpete_frame;

/* The pool walk's own shape, held here rather than in the descriptor because
 * it is the shape of one routine: the selector's class field and the sentinel
 * that ends the pool are how the routine dispatches, not a per-game datum. */
#define PART_CLASS_SHIFT 8
#define PART_CLASS_MASK  0xF
#define PART_POOL_END    (-1)   /* selector >> 8 at the end-of-pool sentinel */

/* Pool slots the allocation serials cover; the Spyro 2 and 3 pools are 0x2000
 * bytes of 0x20-byte records, 256 slots. */
#define PART_SLOTS_MAX 1024u

/* Per pool slot, the serial of the allocation that last handed it out (0 for
 * none seen since the pool base was last read, or freed since). The serial is
 * the allocation's tick and ordinal (alloc_serial_next), so a tick re-run from
 * a restored state stamps the serials its first run did. The record carries nothing that
 * names the allocation it came from: the allocator writes only the class byte
 * (+0x01), which a slot reused by a spawn of the same class keeps, and every
 * other byte is the spawner's and the type's update's to write, the byte at
 * +0x02 counted up by some types, down by others and used as a bit field by
 * others. The serial is the allocation itself, observed where the game makes
 * it. */
static uint32_t s_slot_serial[PART_SLOTS_MAX];
static uint32_t s_slot_base;      /* the pool base the serials index from */
static alloc_serial_t s_alloc_serial;   /* the pool's serial source */
static int      s_alloc_in;       /* the allocator hook's re-entrancy latch */
static unsigned long s_free_hits; /* free-hook calls that cleared a slot's serial */

#define PART_MAX_PRIMS 2048

/* The class a two-point record draws as: the routine projects a second record
 * point for it and the primitive's two endpoints are those two projections
 * (partsdraw.s:650-659 reads the high half of +0x8 for the second X and +0xC
 * for its Y and Z, then stores the two screen points at polybuf +0x8 and +0x10).
 * Every other paired class carries one record point.
 *
 * `PART_ROT_CLASS` is the class whose corner extents are a rotation of a fixed
 * pair of view-space offsets around the record's own view position, all four
 * corners sharing that position's Z (partsdraw.s:495-504 reads the angle,
 * :527-538 builds the four offsets from the sine pair and :567-593 projects
 * them with VZ 0 against the record's translation). PART_RECT_CLASS is a
 * screen-aligned rectangle about the projected point (partsdraw.s:388-446);
 * PART_SPIN_CLASS spins a square in the world's horizontal plane and projects
 * each corner on its own Z (:225-316); PART_DOT_CLASS is a two-pixel line at
 * the projected point (:709-748). part_build builds each class's vertices. */
#define PART_DOT_CLASS       0
#define PART_TWO_POINT_CLASS 1
#define PART_ROT_CLASS       2
#define PART_RECT_CLASS      3
#define PART_SPIN_CLASS      4

typedef struct {
    ngte_prim p;
    float     depth;
    double    vz;         /* the record point's view-Z under the tick's camera */
    float     bx[4], by[4]; /* the vertices part_build builds from the tick's state */
    uint8_t   nb;         /* how many of bx/by part_build filled: 4, 2, or 0 */
    uint8_t   verr_n;     /* vertices in verr, set by the canonical feed's vertex census */
    float     verr[4][2]; /* drawn minus built, per vertex, on the canonical feed */
    double    psx, psy;   /* the record's own projected point, or the sentinel */
    double    psx2, psy2; /* the second record point of a two-point class */
    int       cls;        /* the record's class, or -1 when unpaired */
    uint32_t  rec;        /* the record that appended it, 0 when unpaired */
    uint32_t  w0, w1, w2; /* the record's position words as the tick read them */
    int       owner;      /* player_hide_particle_owner of the record that appended it */
    uint32_t  serial;     /* the record's allocation serial at the draw (particles_slot_serial) */
} part_item;

/* The rebase of one captured primitive onto the sub-tick pose: its vertices
 * rebuilt at the state the present shows, and the depth of the record point
 * there. Inactive on a canonical feed; at the end of the tick's own interval
 * the shown state and the staged camera are the tick's, so the vertices equal
 * the capture's. */
typedef struct {
    float dx, dy;         /* the record point's screen displacement (debug lines) */
    float depth;          /* the depth of the re-projected record point */
    uint8_t active;       /* 1 = draw bx/by (when nb) and take `depth` */
    uint8_t dsz;          /* 1 = the shown size byte differs from the tick's */
    uint8_t nb;           /* how many of bx/by part_build filled at the shown state */
    uint8_t col_on;       /* 1 = feed col[] in place of the primitive's colour words */
    uint8_t verdict;      /* part_record_pair's verdict (PART_PAIR_*) */
    float bx[4], by[4];
    double shown[5];      /* the shown record point (X, Y, Z halfwords), size and angle bytes */
    uint32_t col[2];      /* the shown colour words, 24-bit, per endpoint (one for a polygon) */
} part_rebase;

static part_item   s_item_buf[2][PART_MAX_PRIMS];
static part_item*  s_item = s_item_buf[0];
static part_rebase s_reb[PART_MAX_PRIMS];
static int         s_item_n;
/* The canonical epoch the captured set was taken in and the object-capture
 * stamp current then (every pool state that draw read is stamped at or before
 * it), and the previous epoch's set, which a held camera cut replays
 * (particles_replay_prev). */
static uint32_t  s_item_epoch;
static op_time_t s_item_ref_ns;
static struct { part_item* item; int n; uint32_t epoch; op_time_t ref_ns; } s_item_prev = { .item = s_item_buf[1] };
/* While the previous set is replayed: its records show each pool slot's state
 * as of that set's draw, the pair's prev for a slot that rolled after
 * s_item_ref_cut and its curr otherwise. */
static int       s_item_ref_prev;
static op_time_t s_item_ref_cut;
/* The set a held or late present shows (cut_held_set_pick), or -1 outside one. */
static int s_item_held = -1;

/** @brief Exchange the captured set, its epoch and stamp for the previous epoch's. */
static void part_item_swap(void) {
    part_item* t = s_item; s_item = s_item_prev.item; s_item_prev.item = t;
    int n = s_item_n; s_item_n = s_item_prev.n; s_item_prev.n = n;
    uint32_t e = s_item_epoch; s_item_epoch = s_item_prev.epoch; s_item_prev.epoch = e;
    op_time_t r = s_item_ref_ns; s_item_ref_ns = s_item_prev.ref_ns; s_item_prev.ref_ns = r;
}

static uint32_t s_code_hist[256];
static uint32_t s_tag_hist[256];
static uint32_t s_fed_tris;      /* native triangles the last feed emitted */

/** @brief The ngte_prim_sink that appends a captured primitive to s_item.
 *
 * The new item is unpaired: depth 1 (the near guard), class -1, no record, no
 * built vertices and an inactive rebase.
 * @param s The decoded primitive.
 * @return 1 when stored, 0 when s_item holds PART_MAX_PRIMS. */
static int part_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= PART_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 1.0f;   /* unpaired: the near guard (view-Z 1) */
    s_item[s_item_n].psx = s_item[s_item_n].psy = 0.0;
    s_item[s_item_n].psx2 = s_item[s_item_n].psy2 = 0.0;
    s_item[s_item_n].cls = -1;
    s_item[s_item_n].rec = 0u;
    s_item[s_item_n].w0 = s_item[s_item_n].w1 = s_item[s_item_n].w2 = 0u;
    s_item[s_item_n].owner = 0;
    s_item[s_item_n].serial = 0u;
    s_item[s_item_n].nb = 0;
    s_item[s_item_n].verr_n = 0;
    memset(&s_reb[s_item_n], 0, sizeof s_reb[0]);
    s_item_n++;
    return 1;
}

/** @brief Captures the primitives the routine appended to the polybuf interval [start, end) into s_item.
 *
 * Resets the captured set, then walks with ngte_walk_prims. The longest
 * primitive the routine appends is the ten-word POLY_FT4 (tag length 9), so a
 * longer tag ends the walk; every code is kept with its first ten raw words,
 * since a line is not a polygon and the line feed decodes it from those words.
 * The code and tag histograms count the stored primitives. At most
 * PART_MAX_PRIMS are kept.
 * @param cpu Reads guest RAM.
 * @param start Guest address of the first primitive.
 * @param end Guest address one past the last primitive. */
static void part_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) OP_REQUIRES(sim) {
    /* The first capture of a new canonical epoch retires the current set. */
    const uint32_t epoch = spyro_native_canon_epoch_stamp();
    if (s_item_epoch != epoch) part_item_swap();
    s_item_epoch  = epoch;
    s_item_ref_ns = atomic_load_explicit(&g_openpete_tick_stamp, memory_order_relaxed);
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 9, .raw_words_max = 10, .guard = PART_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, part_prim_sink, NULL);
}

/** @brief Unpacks one record point from the pool's position words, displaced by `off`.
 *
 * The first point is X in the low half and Y in the high half of the first
 * word, Z in the low half of the second (partsdraw.s:709-712), each an unsigned
 * halfword in quarter world units. A two-point class's second point occupies
 * the next three halfwords: X in the high half of the second word
 * (partsdraw.s:650-651 subtracts the camera X from it), Y in the low half and Z
 * in the high half of the third (partsdraw.s:653-659 subtracts the camera Y and
 * Z from those two).
 * @param it The captured primitive holding the record's position words.
 * @param second Nonzero selects the second point.
 * @param off Displacement in the pool's quarter world units, or NULL for none.
 * @param p Receives the point (X, Y, Z). */
static void part_rec_point(const part_item* it, int second, const double off[3],
                           double p[3]) {
    if (second) {
        p[0] = (double)((it->w1 >> 16) & 0xFFFFu);
        p[1] = (double)(it->w2 & 0xFFFFu);
        p[2] = (double)((it->w2 >> 16) & 0xFFFFu);
    } else {
        p[0] = (double)(it->w0 & 0xFFFFu);
        p[1] = (double)((it->w0 >> 16) & 0xFFFFu);
        p[2] = (double)(it->w1 & 0xFFFFu);
    }
    if (off) { p[0] += off[0]; p[1] += off[1]; p[2] += off[2]; }
}

/** @brief Wraps a camera-relative difference in quarter world units into [-32768, 32768), as the GTE's int16 vector load does.
 *
 * The routine subtracts each unsigned record halfword from the camera
 * position shifted right by 2 and loads the difference into VXY0 and VZ0,
 * whose components are int16 (Spyro 2 partsdraw.s:641-664, :709-722), so a
 * point whose difference passes 0x8000 lands on the other side of the camera
 * by 0x10000 units. A difference already in range is returned unchanged.
 * @param d The difference, fractional under a sub-tick camera.
 * @return The difference modulo 65536 in [-32768, 32768). */
static double part_wrap16(double d) {
    if (d >= -32768.0 && d < 32768.0) return d;
    return d - 65536.0 * floor((d + 32768.0) / 65536.0);
}

/** @brief Projects a pool point the way the routine does.
 *
 * The GTE vector is (camY - Y, camZ - Z, X - camX) with the camera position
 * divided by four into the pool's quarter world units, each component wrapped
 * to int16 as the GTE loads it (part_wrap16), rotated by the camera matrix at
 * +0x00 with a zero translation, through the GTE-faithful projection without
 * the SXY bound.
 * @param v The view; supplies the camera matrix, position and offsets.
 * @param p The point (X, Y, Z) in quarter world units.
 * @param o Receives the projection; o->m3 is the view-Z in quarter units. */
static void part_project(const ngte_view* v, const double p[3], ngte_proj* o) {
    const double cx = v->cam[0] / 4.0, cy = v->cam[1] / 4.0, cz = v->cam[2] / 4.0;
    ngte_project_gte(v->cam_M, NULL, part_wrap16(cy - p[1]), part_wrap16(cz - p[2]),
                     part_wrap16(p[0] - cx), v->ofx, v->ofy, v, 0, o);
}

/** @brief Builds a PART_ROT_CLASS billboard's four corners in float, as the routine builds them in integers.
 *
 * The routine moves the record point's view-space MACs, shifted left by 2,
 * into the GTE translation (partsdraw.s:529-534), sets the rotation to
 * diag(0x1000, 0xA00, 0x1000) (:567-571), and projects four corners with VZ 0
 * (RTPT at :574 and RTPS at :593): (-a2, -a3), (a3, -a2), (-a3, a2), (a2, a3),
 * where a2 and a3 are the size byte times the sine-table entry at angle + 32
 * and at angle + 96, shifted right by 10 (:495-504, :527, :537-538). The GTE
 * truncates each corner's MACs and screen position to integers, so a distant
 * billboard whose extent spans a few pixels jumps by up to a pixel on every
 * tick independent of its motion. Here every term stays fractional: the
 * translation from the point's untruncated MACs, the table entry
 * interpolated at a fractional angle (ngte_sincos_f), the product without the
 * shift's truncation, and the projection without the screen rounding. The
 * corner order is the primitive's vertex order (:594-596 store the RTPT's
 * SXY0..2, :612 the RTPS's).
 * @param cpu Reads the sine table.
 * @param v The view the corners project under.
 * @param t The translation: the record point's view MACs times 4.
 * @param size The size byte, fractional on a sub-tick present.
 * @param ang The angle byte, fractional on a sub-tick present.
 * @param x Receives the four screen X.
 * @param y Receives the four screen Y. */
static void part_rot_corners(CPUState* cpu, const ngte_view* v, const double t[3],
                             double size, double ang, float x[4], float y[4]) {
    double s, c;
    ngte_sincos_f(cpu, ang + 32.0, &s, &c);
    const double a2 = size * s / 1024.0, a3 = size * c / 1024.0;
    const double ox[4] = { -a2, a3, -a3, a2 };
    const double oy[4] = { -a3, -a2, a2, a3 };
    for (int k = 0; k < 4; k++) {
        ngte_proj o;
        ngte_screen_gte(t[0] + ox[k], t[1] + oy[k] * (2560.0 / 4096.0), t[2],
                        v->ofx, v->ofy, v, 0, &o);
        x[k] = (float)o.sx;
        y[k] = (float)o.sy;
    }
}

/** @brief Builds a PART_RECT_CLASS sprite's four corners in float: a screen-aligned rectangle centred on the record point.
 *
 * The routine projects the record point (RTPS at partsdraw.s:388), reads the
 * depth cue MAC0 = DQA · (H / SZ3) with DQA 0x100 and DQB 0 (:49-51), which
 * is the RTPS quotient in 16.16 times 0x100, shifts it right by 12 into IR0
 * (:416-419) so IR0 holds the quotient in Q12, multiplies IR0 by the size
 * byte and by the top byte of the second position word (:392-396, :423) and
 * shifts the MACs right by 12 (:428-431): the width and the height in pixels,
 * each the byte times the projection's own quotient. It then places the
 * corners at the point plus half of each, shifted right by 1, minus the whole
 * (:432-446). Both shifts truncate, so the rectangle sits up to a pixel and a
 * half off its point and its extent steps by whole pixels. Here the extent is
 * the byte times ngte_gte_quot, the quotient the point's own projection uses
 * (the divide cap and the --fov scale included), and the rectangle is centred
 * exactly on the point. The corner order is the primitive's: top-left,
 * top-right, bottom-left, bottom-right (:446, :444, :442, :440).
 * @param v The view the point projected under.
 * @param pt The record point's projection.
 * @param size The size byte, fractional on a sub-tick present.
 * @param hgt The top byte, fractional on a sub-tick present.
 * @param x Receives the four screen X.
 * @param y Receives the four screen Y. */
static void part_rect_corners(const ngte_view* v, const ngte_proj* pt, double size,
                              double hgt, float x[4], float y[4]) {
    const double q = ngte_gte_quot(v, pt->m3);
    const double hw = size * q / 2.0, hh = hgt * q / 2.0;
    const double cx[4] = { -hw, hw, -hw, hw };
    const double cy[4] = { -hh, -hh, hh, hh };
    for (int j = 0; j < 4; j++) {
        x[j] = (float)(pt->sx + cx[j]);
        y[j] = (float)(pt->sy + cy[j]);
    }
}

/** @brief Builds a PART_SPIN_CLASS quad's four corners in float: a square spun in the world's horizontal plane about the record point.
 *
 * The routine multiplies the size byte by the sine-table entries at the angle
 * byte and at angle + 64 (partsdraw.s:225-241), shifts the products right by
 * 11 (:248-249) into a1 and a0, and projects four world points through the
 * camera with zero translation (RTPT at :267, RTPS at :285): the record point
 * with its first GTE component (camera Y - Y) displaced by -a1, a0, -a0, a1
 * and its third (X - camera X) by a0, a1, -a1, -a0 (:250-266). Each corner has
 * its own depth, so the quad is not a screen-space rotation of the captured
 * one. Here the products keep their fraction and each corner goes through the
 * float projection of part_project. The corner order is the primitive's
 * (:313-316).
 * @param cpu Reads the sine table.
 * @param v The view the corners project under.
 * @param p The record point, quarter world units.
 * @param size The size byte, fractional on a sub-tick present.
 * @param ang The angle byte, fractional on a sub-tick present.
 * @param x Receives the four screen X.
 * @param y Receives the four screen Y. */
static void part_spin_corners(CPUState* cpu, const ngte_view* v, const double p[3],
                              double size, double ang, float x[4], float y[4]) {
    double s, c;
    ngte_sincos_f(cpu, ang, &s, &c);
    const double a1 = size * s / 2048.0, a0 = size * c / 2048.0;
    const double d0[4] = { -a1, a0, -a0, a1 };   /* added to camera Y - Y */
    const double d2[4] = { a0, a1, -a1, -a0 };   /* added to X - camera X */
    for (int k = 0; k < 4; k++) {
        const double q[3] = { p[0] + d2[k], p[1] - d0[k], p[2] };
        ngte_proj o;
        part_project(v, q, &o);
        x[k] = (float)o.sx;
        y[k] = (float)o.sy;
    }
}

/* The state a record shows on a present: the tick's own on a canonical feed,
 * the pair's interpolation on a sub-tick one. The two bytes above the first
 * point's Z are read by class: byte 2 is every billboard's size; byte 3 is an
 * angle for PART_ROT_CLASS and PART_SPIN_CLASS and the height of
 * PART_RECT_CLASS, so it is carried both ways. */
typedef struct {
    double off[3];   /* the record points' displacement from the captured words */
    double size;     /* byte 2 of the second position word */
    double ang;      /* byte 3, lerped along the shortest arc of the 256-step circle */
    double hgt;      /* byte 3, lerped as a magnitude */
    uint32_t col[2]; /* the shown colour words, 24-bit, per endpoint; valid when col_on */
    int col_on;      /* 1 when col replaces the primitive's colour words */
} part_state;

/** @brief The state the captured words hold: no displacement, the tick's own bytes.
 *
 * @param it The paired captured primitive.
 * @param st Receives the state. */
static void part_state_tick(const part_item* it, part_state* st) {
    st->off[0] = st->off[1] = st->off[2] = 0.0;
    st->size = (double)((it->w1 >> 16) & 0xFFu);
    st->ang = st->hgt = (double)((it->w1 >> 24) & 0xFFu);
    st->col[0] = st->col[1] = 0u;
    st->col_on = 0;
}

/** @brief Builds a fixed-layer billboard's four corners in float: the PART_ROT_CLASS offsets about a view-space point the record holds.
 *
 * The routine's fixed-layer path takes no camera term: it loads the GTE
 * translation straight from the record, TRX = s16(+0x4) << 2, TRY =
 * word(+0x4) >> 14 arithmetic and TRZ = s16(+0x8) << 2 (partsdraw.s:89,
 * :111-119), builds the corner offsets from the size byte +0xA and the
 * sine-table entries at angle byte +0xB plus 32 and plus 96 (:97-110,
 * :120-123), and projects them under diag(0x1000, 0xA00, 0x1000) (:150-156,
 * RTPT :159, RTPS :174), the PART_ROT_CLASS corners about that translation.
 * The halfwords are signed view coordinates; TRY keeps the two bits its shift
 * takes from the top of +0x4's low half, and the shown displacement `off`,
 * in halfword units, scales by four as the halfwords do. The projection is the canonical one
 * (ngte_view_canon): the point is where the game placed the element in its
 * own view, so --fov does not scale it.
 * @param cpu Reads the sine table.
 * @param v The view; supplies the screen offsets and the canonical H.
 * @param it The paired captured primitive holding the record's words.
 * @param st The state shown.
 * @param x Receives the four screen X.
 * @param y Receives the four screen Y. */
static void part_fixed_corners(CPUState* cpu, const ngte_view* v, const part_item* it,
                               const part_state* st, float x[4], float y[4]) {
    ngte_view c;
    ngte_view_canon(v, &c);
    const double t[3] = {
        4.0 * ((double)(int16_t)(it->w0 & 0xFFFFu) + st->off[0]),
        (double)((int32_t)it->w0 >> 14) + 4.0 * st->off[1],
        4.0 * ((double)(int16_t)(it->w1 & 0xFFFFu) + st->off[2]),
    };
    part_rot_corners(cpu, &c, t, st->size, st->ang, x, y);
}

/** @brief Builds a paired primitive's vertices in float from its record's state, as its class's routine builds them.
 *
 * A polygon class yields the four corners in the primitive's vertex order.
 * PART_DOT_CLASS yields the record point and the point one pixel right of it
 * (partsdraw.s:746-748 store SXY and SXY + 1); PART_TWO_POINT_CLASS yields
 * the two record points' projections (:683-684); a class of
 * fixed_layer_class and up yields part_fixed_corners' four. Nothing is built
 * when the captured primitive's shape is not the one its class appends.
 * @param cpu Reads the sine table.
 * @param v The view the vertices project under.
 * @param it The paired captured primitive.
 * @param st The state shown.
 * @param x Receives the screen X.
 * @param y Receives the screen Y.
 * @return The vertex count: 4, 2, or 0 when nothing was built. */
static int part_build(CPUState* cpu, const ngte_view* v, const part_item* it,
                      const part_state* st, float x[4], float y[4]) {
    const ngte_prim* pr = &it->p;
    const int quad = pr->nv == 4;
    const int line = pr->nv == 0 && ((pr->code & 0xF8u) == 0x40u || (pr->code & 0xF8u) == 0x50u);
    double t[3];
    double p[3];
    ngte_proj o;
    if ((uint32_t)it->cls >= game_layout.particles.fixed_layer_class) {
        if (!quad) return 0;
        part_fixed_corners(cpu, v, it, st, x, y);
        return 4;
    }
    part_rec_point(it, 0, st->off, p);
    part_project(v, p, &o);
    switch (it->cls) {
    case PART_DOT_CLASS:
        if (!line) return 0;
        x[0] = (float)o.sx; y[0] = (float)o.sy;
        x[1] = (float)(o.sx + 1.0); y[1] = (float)o.sy;
        return 2;
    case PART_TWO_POINT_CLASS: {
        if (!line) return 0;
        ngte_proj o2;
        part_rec_point(it, 1, st->off, p);
        part_project(v, p, &o2);
        x[0] = (float)o.sx; y[0] = (float)o.sy;
        x[1] = (float)o2.sx; y[1] = (float)o2.sy;
        return 2;
    }
    case PART_ROT_CLASS:
        if (!quad) return 0;
        t[0] = 4.0 * o.m1; t[1] = 4.0 * o.m2; t[2] = 4.0 * o.m3;
        part_rot_corners(cpu, v, t, st->size, st->ang, x, y);
        return 4;
    case PART_RECT_CLASS:
        if (!quad) return 0;
        part_rect_corners(v, &o, st->size, st->hgt, x, y);
        return 4;
    case PART_SPIN_CLASS:
        if (!quad) return 0;
        part_spin_corners(cpu, v, p, st->size, st->ang, x, y);
        return 4;
    default:
        return 0;
    }
}

/* Records the last pairing walk found stamped with no primitive appended. */
static int s_stamp_skipped;

/* The GTE rotation and translation the routine projects the next record point
 * under. The routine loads the camera rotation with a zero translation at
 * entry (Spyro 2 partsdraw.s:35-42). A class-2 record that passes its window
 * moves its point's MACs << 2 into the translation (:531-536), saves the
 * rotation (:562-566) and sets diag(0x1000, 0xA00, 0x1000) (:567-573); the
 * append path restores the saved rotation and zeroes the translation
 * (:613-620), but the key skip at :582 branches back to the walk before that
 * restore, so the records after it project under the diagonal rotation and
 * the skipped record's translation. The next class-2 append or fixed-layer
 * record (:147-151, :196-203) zeroes the translation and restores the
 * rotation it saved, which after such a skip is the diagonal, so the camera
 * rotation does not return until the routine's next call. Spyro 3 clamps the
 * class-2 key and never skips, so its state stays the camera's. */
typedef struct {
    int R[3][3];
    int tr[3];
} part_gte;

static const int k_part_rot_diag[3][3] = {
    { 0x1000, 0, 0 }, { 0, 0xA00, 0 }, { 0, 0, 0x1000 },
};

/** @brief Report whether the pairing walk models the GTE state a class-2 key skip carries.
 *
 * OPENPETE_PART_GTE_CARRY=0 pairs under the camera state for every record,
 * ignoring the carry: the A/B side of the pairing audit.
 * @return 1 unless OPENPETE_PART_GTE_CARRY is 0. */
static int part_gte_carry_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_PART_GTE_CARRY"); on = !(e && *e == '0'); }
    return on;
}

/* Run totals: class-2 key skips the walk modelled, stamped records it
 * predicted under a state other than the camera's, and the subset of those
 * under the diagonal rotation with a zero translation (past a later class-2
 * append or fixed-layer record). */
static unsigned long s_gte_skips, s_gte_carried, s_gte_rot_only;
/* Carried records whose skip verdict under the camera state differs: the
 * records a camera-state pairing walk misaligns on. */
static unsigned long s_gte_flips;

/** @brief Whether `g` differs from the camera state the routine loads at entry.
 *
 * @param g The modelled state.
 * @param v The tick's view; supplies the camera matrix.
 * @return 1 when the rotation or the translation is not the camera's. */
static int part_gte_carried(const part_gte* g, const ngte_view* v) {
    return memcmp(g->R, v->cam_M, sizeof g->R) != 0 || g->tr[0] || g->tr[1] || g->tr[2];
}

/** @brief RTPS of one pool point under `g`, loaded as the routine loads it.
 *
 * VZ0 is X minus the camera X; VXY0 is the camera Y minus Y plus the camera Z
 * minus Z shifted left by 16, one 32-bit add, so a negative low half borrows
 * one from VY0 (Spyro 2 partsdraw.s:482-491, :709-722); the camera position is
 * shifted right by 2 (:45-50).
 * @param g The modelled GTE state.
 * @param v The tick's view; supplies the camera RAM position, offsets and H.
 * @param x The point's X, quarter world units.
 * @param y The point's Y.
 * @param z The point's Z.
 * @param o Receives the GTE's output. */
static void part_gte_rtps(const part_gte* g, const ngte_view* v, int32_t x, int32_t y,
                          int32_t z, ngte_rtps_out* o) {
    const int32_t cx = (int32_t)v->cam_ram[0] >> 2;
    const int32_t cy = (int32_t)v->cam_ram[1] >> 2;
    const int32_t cz = (int32_t)v->cam_ram[2] >> 2;
    const uint32_t vxy = (uint32_t)(cy - y) + ((uint32_t)(cz - z) << 16);
    ngte_rtps_int(g->R, g->tr, (int16_t)(vxy & 0xFFFFu), (int16_t)(vxy >> 16),
                  (int16_t)(x - cx), v->ofx, v->ofy, (int)v->H_canon, o);
}

/** @brief Whether a stamped record failed its class's test between the stamp and the append, so the routine appended nothing for it.
 *
 * Reproduces the routine's test from game_layout.particles.stamp_skip: the
 * ordering key (SZ3 >> 5) - size, from the record point's RTPS under the GTE
 * state the routine holds at that record (part_gte_rtps), is compared against
 * the class's key_min.
 * @param cpu Reads the record's size byte.
 * @param rec The stamped record's guest address.
 * @param cls The record's class, below fixed_layer_class.
 * @param o The record point's RTPS under the routine's state.
 * @return 1 when the routine stamped the record and appended nothing. */
static int part_stamp_skipped(CPUState* cpu, uint32_t rec, int cls, const ngte_rtps_out* o) {
    const struct game_part_stamp_skip* t = &game_layout.particles.stamp_skip[cls];
    if (!t->on) return 0;
    const int size = t->size_off ? (int)cpu->read_byte(rec + t->size_off) : 0;
    return (o->sz3 >> 5) - size < t->key_min;
}

/** @brief Advances the modelled GTE state past a stamped record of class cls.
 *
 * A PART_ROT_CLASS record that skipped leaves diag(0x1000, 0xA00, 0x1000) and
 * its point's MACs << 2; one that appended, and a fixed-layer record, zero the
 * translation and keep the rotation. Every other class leaves the state.
 * @param g The modelled state.
 * @param cls The record's class.
 * @param skipped Whether the record failed its key test.
 * @param o The record point's RTPS under `g`. */
static void part_gte_step(part_gte* g, int cls, int skipped, const ngte_rtps_out* o) {
    if ((uint32_t)cls >= game_layout.particles.fixed_layer_class) {
        g->tr[0] = g->tr[1] = g->tr[2] = 0;
        return;
    }
    if (cls != PART_ROT_CLASS) return;
    if (!skipped) { g->tr[0] = g->tr[1] = g->tr[2] = 0; return; }
    if (!part_gte_carry_on()) return;
    memcpy(g->R, k_part_rot_diag, sizeof g->R);
    for (int k = 0; k < 3; k++) g->tr[k] = (int)((uint32_t)o->mac[k] << 2);
    s_gte_skips++;
}

static int part_vdbg_on(void);
static int s_vdbg;

/* OPENPETE_PART_VERTEX_DBG: the pairing audit, per class. `n` paired
 * primitives were re-run through the routine's integer path under the
 * modelled GTE state; `id` of them carry a colour word other than their
 * record's, `pos` a vertex other than the re-run's; `car` were predicted
 * under a carried state. id + pos of zero with car above zero proves the
 * pairing followed the guest across a carry. */
static unsigned long s_aud[4][16];   /* 0 n, 1 id, 2 pos, 3 car */

/** @brief The packed screen word of a captured primitive's vertex k: a POLY_FT4's corner, or a line's endpoint.
 *
 * @param p The captured primitive.
 * @param k The vertex index.
 * @return The XY word as the routine stored it. */
static uint32_t part_raw_xy(const ngte_prim* p, int k) {
    if (p->nv) return p->raw[2u + 2u * (uint32_t)k];
    const uint32_t words = (p->code & 0x10u) ? 3u : 2u;
    return p->raw[2u + (uint32_t)k * (words - 1u)];
}

/** @brief The SXY word of an RTPS output, as mfc2 of SXY2 reads it.
 *
 * @param o The RTPS output.
 * @return SX in the low half, SY in the high half. */
static uint32_t part_sxy(const ngte_rtps_out* o) {
    return (uint32_t)(uint16_t)o->sx | ((uint32_t)(uint16_t)o->sy << 16);
}

/** @brief RTPS of a PART_SPIN_CLASS record's four corners under `g`, as the routine projects them.
 *
 * The size byte times the sine-table entries at the angle byte and at angle +
 * 64, shifted right by 11, displace the first GTE component (camera Y - Y) by
 * -a1, a0, -a0, a1 and the third (X - camera X) by a0, a1, -a1, -a0 (Spyro 2
 * partsdraw.s:227-251 form a1 and a0, :252-270 the corners); the first two
 * components are one packed 32-bit add (:235-237, :253-254). RTPT projects
 * corners 0-2 and RTPS corner 3 (:269, :287).
 * @param cpu Reads the sine table.
 * @param v The tick's view; supplies the camera RAM position, offsets and H.
 * @param g The modelled GTE state.
 * @param w0 The record's first position word.
 * @param w1 The record's second position word (Z low, size and angle bytes high).
 * @param c Receives the four corners' GTE output. */
static void part_spin_rtps(CPUState* cpu, const ngte_view* v, const part_gte* g,
                           uint32_t w0, uint32_t w1, ngte_rtps_out c[4]) {
    const uint32_t tbl = game_layout.sin_table;
    const int32_t cx = (int32_t)v->cam_ram[0] >> 2;
    const int32_t cy = (int32_t)v->cam_ram[1] >> 2;
    const int32_t cz = (int32_t)v->cam_ram[2] >> 2;
    const uint32_t size = (w1 >> 16) & 0xFFu;
    const uint32_t ia = (w1 >> 23) & 0x1FEu;
    const int sn = (int16_t)cpu->read_half(tbl + ia), cs = (int16_t)cpu->read_half(tbl + ia + 0x80u);
    const int a1 = ((int)size * sn) >> 11, a0 = ((int)size * cs) >> 11;
    const int32_t vz = (int32_t)(w0 & 0xFFFFu) - cx;
    const int32_t vx = cy - (int32_t)(w0 >> 16);
    const uint32_t vy = (uint32_t)(cz - (int32_t)(w1 & 0xFFFFu)) << 16;
    const int dxy[4] = { -a1, a0, -a0, a1 }, dz[4] = { a0, a1, -a1, -a0 };
    for (int k = 0; k < 4; k++) {
        const uint32_t p = (uint32_t)(vx + dxy[k]) + vy;
        ngte_rtps_int(g->R, g->tr, (int16_t)(p & 0xFFFFu), (int16_t)(p >> 16),
                      (int16_t)(vz + dz[k]), v->ofx, v->ofy, (int)v->H_canon, &c[k]);
    }
}

/** @brief Re-runs a paired record's vertices through the routine's integer path under the modelled GTE state.
 *
 * Spyro 2 partsdraw.s: class 0 stores SXY and SXY + 1 (:746-748); class 1
 * the two points' SXY (:683-684); class 2 projects (-a2, -a3), (a3, -a2),
 * (-a3, a2), (a2, a3) with VZ 0 under diag(0x1000, 0xA00, 0x1000) and its
 * point's MACs << 2 (:531-596), a2 and a3 the size times the table entries at
 * angle + 32 and + 96 shifted right by 10; class 3 places the corners at
 * SXY + (w >> 1, h >> 1) minus w and h, each the IR0 = (0x100 * quotient)
 * >> 12 times the byte, shifted right by 12 (:416-448); class 4 projects the
 * point displaced by the size times the table entries at the angle and
 * angle + 64, shifted right by 11 (:225-318). All packed adds are 32-bit.
 * @param cpu Reads the sine table.
 * @param v The tick's view.
 * @param g The state the record point projected under.
 * @param it The paired item, position words filled.
 * @param o The record point's RTPS under `g`.
 * @param xy Receives the packed vertex words.
 * @return The vertex count, 0 for a class the re-run does not cover. */
static int part_int_verts(CPUState* cpu, const ngte_view* v, const part_gte* g,
                          const part_item* it, const ngte_rtps_out* o, uint32_t xy[4]) {
    const uint32_t sxy = part_sxy(o);
    const uint32_t size = (it->w1 >> 16) & 0xFFu, top = it->w1 >> 24;
    const uint32_t tbl = game_layout.sin_table;
    switch (it->cls) {
    case PART_DOT_CLASS:
        xy[0] = sxy; xy[1] = sxy + 1u;
        return 2;
    case PART_TWO_POINT_CLASS: {
        ngte_rtps_out o2;
        part_gte_rtps(g, v, (int32_t)(it->w1 >> 16), (int32_t)(it->w2 & 0xFFFFu),
                      (int32_t)(it->w2 >> 16), &o2);
        xy[0] = sxy; xy[1] = part_sxy(&o2);
        return 2;
    }
    case PART_ROT_CLASS: {
        const uint32_t ia = ((it->w1 >> 23) + 0x40u) & 0x1FEu;
        const int s1 = (int16_t)cpu->read_half(tbl + ia), s2 = (int16_t)cpu->read_half(tbl + ia + 0x80u);
        const int a2 = ((int)size * s1) >> 10, a3 = ((int)size * s2) >> 10;
        const int ox[4] = { -a2, a3, -a3, a2 }, oy[4] = { -a3, -a2, a2, a3 };
        int tr[3];
        for (int k = 0; k < 3; k++) tr[k] = (int)((uint32_t)o->mac[k] << 2);
        for (int k = 0; k < 4; k++) {
            ngte_rtps_out c;
            ngte_rtps_int(k_part_rot_diag, tr, (int16_t)ox[k], (int16_t)oy[k], 0,
                          v->ofx, v->ofy, (int)v->H_canon, &c);
            xy[k] = part_sxy(&c);
        }
        return 4;
    }
    case PART_RECT_CLASS: {
        const uint32_t ir0 = ((uint32_t)o->q * 0x100u) >> 12;
        const uint32_t w = (ir0 * size) >> 12, h = (ir0 * top) >> 12;
        const uint32_t c = sxy + (w >> 1) + ((h >> 1) << 16);
        xy[3] = c; xy[2] = c - w; xy[1] = c - (h << 16); xy[0] = xy[1] - w;
        return 4;
    }
    case PART_SPIN_CLASS: {
        ngte_rtps_out c[4];
        part_spin_rtps(cpu, v, g, it->w0, it->w1, c);
        for (int k = 0; k < 4; k++) xy[k] = part_sxy(&c[k]);
        return 4;
    }
    default:
        return 0;
    }
}

/** @brief Adds a paired primitive to the pairing audit: its colour word against its record's, and its vertices against the integer re-run under the modelled state.
 *
 * The colour word the routine copies is the record's +0x10 for class 1
 * (Spyro 2 partsdraw.s:668, :685) and +0xC for the other classes (:164, :272,
 * :435, :579, :726). At OPENPETE_PART_VERTEX_DBG 2 or more a mismatch prints
 * one line.
 * @param cpu Reads the record and the sine table.
 * @param v The tick's view.
 * @param g The state the record point projected under.
 * @param it The paired item.
 * @param o The record point's RTPS under `g`. */
static void part_pair_audit(CPUState* cpu, const ngte_view* v, const part_gte* g,
                            const part_item* it, const ngte_rtps_out* o) {
    if (!part_vdbg_on()) return;
    const int c = it->cls & 0xF;
    const int car = part_gte_carried(g, v);
    const uint32_t col = cpu->read_word(it->rec + (it->cls == PART_TWO_POINT_CLASS ? 0x10u : 0x0Cu));
    const int id = it->p.raw[1] != col;
    uint32_t xy[4];
    const int nv = part_int_verts(cpu, v, g, it, o, xy);
    int pos = 0;
    for (int k = 0; k < nv; k++) pos |= part_raw_xy(&it->p, k) != xy[k];
    s_aud[0][c]++;
    s_aud[1][c] += (unsigned long)id;
    s_aud[2][c] += (unsigned long)pos;
    s_aud[3][c] += (unsigned long)car;
    if (s_vdbg >= 2 && (id || pos))
        fprintf(stderr, "[part-audit f=%u rec=%08X c=%d car=%d id=%d pos=%d g0=%08X e0=%08X]\n",
                openpete_frame, it->rec, it->cls, car, id, pos, part_raw_xy(&it->p, 0),
                nv ? xy[0] : 0u);
}

/* ---- the routine's per-record verdict ---------------------------------------
 *
 * The routine tests a record's projection against a screen window on the
 * packed SXY word (SY << 16 | SX & 0xFFFF), whose bounds it loads at entry
 * (Spyro 2 partsdraw.s:74-76): the word must exceed 0xD0000 and stay below
 * 0xE40000, and SX << 16 must be positive and below 0x2000000, so SX lies in
 * (0, 512). Classes 0-3 test the record point (class 1 its first point,
 * :669-680; class 0 :733-743; class 2 :509-521; class 3 :403-415); class 4
 * passes when any corner passes each bound (:319-357). In front of the
 * window each class tests SZ3 (classes 0 and 1: nonzero, classes 2 and 3: at
 * least 0x40, all below the window's zmax, 0x2000 in the routine; class 4:
 * the four corners' SZ summed in [0x100, 4 · zmax), :304-310). Classes 1
 * and 4 test their ordering key before the stamp, class 1 (SZ3 >> 5) - byte +0x17 - 2 > 0 (:681, :687-690), class
 * 4 (sum >> 7) - byte +0x11 >= 0 (:271-273, :311-313); classes 0, 2 and 3
 * test it after the stamp (game_layout.particles.stamp_skip). The window is
 * a parameter: PART_WIN_GAME reproduces the routine, a wider, taller or
 * deeper one asks what a view larger than the 4:3 frame, or extended LOD,
 * shows (part_reveal_win). The
 * Spyro 3 routine is the same code (partsdraw.s:72-74). */

/* A screen window on the packed SXY word and on SX, and a depth ceiling. */
typedef struct {
    int32_t top, bottom;   /* the packed word must exceed top and stay below bottom */
    int     xlo, xhi;      /* SX must exceed xlo and stay below xhi */
    int32_t zmax;          /* SZ3 must stay below zmax; a class-4 corner sum below 4 · zmax */
} part_win;

#define PART_WIN_TOP_ROW    13    /* 0xD0000 >> 16 */
#define PART_WIN_BOTTOM_ROW 228   /* 0xE40000 >> 16 */
#define PART_WIN_ZMAX       0x2000
static const part_win PART_WIN_GAME = { PART_WIN_TOP_ROW << 16, PART_WIN_BOTTOM_ROW << 16, 0, 512,
                                        PART_WIN_ZMAX };
/* Bounds no saturated SX or SY (-0x400..0x3FF) reaches. */
#define PART_WIN_FREE_LO (-0x8000)
#define PART_WIN_FREE_HI 0x8000
/* A depth ceiling no saturated SZ3 (0..0xFFFF) reaches, nor four summed. */
#define PART_WIN_ZFREE 0x10000
#define PART_WIN_ROW_CLAMP 0x401

enum { PART_V_STAMP = 1, PART_V_APPEND = 2 };

/** @brief Whether one projected point passes a screen window.
 *
 * @param o The point's GTE output.
 * @param w The window.
 * @return 1 on a pass. */
static int part_win_point(const ngte_rtps_out* o, const part_win* w) {
    const int32_t p = (int32_t)part_sxy(o);
    return p > w->top && p < w->bottom && o->sx > w->xlo && o->sx < w->xhi;
}

/** @brief Whether a PART_SPIN_CLASS record's corners pass a screen window: each bound by at least one corner.
 *
 * @param c The four corners' GTE output.
 * @param w The window.
 * @return 1 on a pass. */
static int part_win_corners(const ngte_rtps_out c[4], const part_win* w) {
    int top = 0, bottom = 0, left = 0, right = 0;
    for (int k = 0; k < 4; k++) {
        const int32_t p = (int32_t)part_sxy(&c[k]);
        top |= p > w->top;
        bottom |= p < w->bottom;
        left |= c[k].sx > w->xlo;
        right |= c[k].sx < w->xhi;
    }
    return top && bottom && left && right;
}

/** @brief One window row scaled about the GTE's OFY by 1 / s, rounded outward and clamped past the saturated SY range.
 *
 * @param row The routine's row bound.
 * @param v The view; supplies ofy and s.
 * @param up 1 rounds toward smaller rows (a top bound), 0 toward larger.
 * @return The scaled row. */
static int part_win_row(int row, const ngte_view* v, int up) {
    const double y = v->ofy + ((double)row - v->ofy) / v->s;
    double r = up ? floor(y) : ceil(y);
    if (r < -PART_WIN_ROW_CLAMP) r = -PART_WIN_ROW_CLAMP;
    if (r > PART_WIN_ROW_CLAMP) r = PART_WIN_ROW_CLAMP;
    return (int)r;
}

/** @brief The window the reveal tests records on: the routine's window widened for a view wider than 4:3 and deepened under extended LOD.
 *
 * In a widened view (ngte_view.extended) the x test is dropped, since
 * part_in_view tests the built primitive against the view's screen-X bounds,
 * and the rows are scaled to the view's height: the routine's rows (13, 228)
 * bound the 4:3 frame's band at the GTE's H; the view projects at H · s, so
 * in the routine's coordinates its band is the routine's scaled about OFY by
 * 1 / s. At s >= 1 the rows are the routine's own packed-word bounds
 * unchanged. Under extended LOD (openpete_no_lod) the depth ceiling is lifted
 * past any SZ3, so a record the routine drops at SZ3 >= 0x2000 (a class-4
 * sum >= 0x8000) passes on its screen window and key tests alone; a view
 * that is not widened keeps the routine's screen window.
 * @param v The view; supplies extended, ofy and s.
 * @return The window. */
static part_win part_reveal_win(const ngte_view* v) {
    part_win w = PART_WIN_GAME;
    if (openpete_no_lod) w.zmax = PART_WIN_ZFREE;
    if (!v->extended) return w;
    w.xlo = PART_WIN_FREE_LO;
    w.xhi = PART_WIN_FREE_HI;
    if (v->s < 1.0) {
        w.top = (int32_t)((uint32_t)part_win_row(PART_WIN_TOP_ROW, v, 1) << 16);
        w.bottom = (int32_t)((uint32_t)part_win_row(PART_WIN_BOTTOM_ROW, v, 0) << 16);
    }
    return w;
}

/** @brief The routine's verdict on a PART_SPIN_CLASS record: its depth sum, its key and its corner window.
 *
 * @param cpu Reads the record and the sine table.
 * @param v The tick's view.
 * @param g The GTE state the routine holds at the record.
 * @param rec The record's guest address.
 * @param w The screen window and depth ceiling.
 * @return PART_V_STAMP | PART_V_APPEND on a pass, else 0. */
static int part_verdict_spin(CPUState* cpu, const ngte_view* v, const part_gte* g,
                             uint32_t rec, const part_win* w) {
    const uint32_t pos = game_layout.particles.pos_off;
    ngte_rtps_out c[4];
    part_spin_rtps(cpu, v, g, cpu->read_word(rec + pos), cpu->read_word(rec + pos + 4u), c);
    const int32_t sum = c[0].sz3 + c[1].sz3 + c[2].sz3 + c[3].sz3;
    if (sum < 0x100 || sum >= 4 * w->zmax) return 0;
    if ((sum >> 7) - (int32_t)cpu->read_byte(rec + 0x11u) < 0) return 0;
    return part_win_corners(c, w) ? (PART_V_STAMP | PART_V_APPEND) : 0;
}

/** @brief The routine's verdict on a record of a class below fixed_layer_class, on a given screen window.
 *
 * Reproduces the class's depth test, screen window and key tests (see the
 * section comment) on the integer RTPS of the record point under `g`, the GTE
 * state the routine holds at the record (part_gte). On PART_WIN_GAME
 * PART_V_STAMP is the stamp the routine writes and PART_V_APPEND whether it
 * appends.
 * @param cpu Reads the record and the sine table.
 * @param v The tick's view.
 * @param g The GTE state the routine holds at the record.
 * @param rec The record's guest address.
 * @param cls The record's class.
 * @param win The screen window and depth ceiling.
 * @return PART_V_STAMP and PART_V_APPEND bits; 0 when the record fails before the stamp. */
static int part_record_verdict(CPUState* cpu, const ngte_view* v, const part_gte* g,
                               uint32_t rec, int cls, const part_win* win) {
    if (cls == PART_SPIN_CLASS) return part_verdict_spin(cpu, v, g, rec, win);
    const uint32_t pos = game_layout.particles.pos_off;
    const uint32_t w0 = cpu->read_word(rec + pos), w1 = cpu->read_word(rec + pos + 4u);
    ngte_rtps_out o;
    part_gte_rtps(g, v, (int32_t)(w0 & 0xFFFFu), (int32_t)(w0 >> 16), (int32_t)(w1 & 0xFFFFu), &o);
    const int32_t zmin = (cls == PART_ROT_CLASS || cls == PART_RECT_CLASS) ? 0x40 : 1;
    if (o.sz3 < zmin || o.sz3 >= win->zmax || !part_win_point(&o, win)) return 0;
    if (cls == PART_TWO_POINT_CLASS)
        return (o.sz3 >> 5) - (int32_t)cpu->read_byte(rec + 0x17u) - 2 > 0
                   ? (PART_V_STAMP | PART_V_APPEND) : 0;
    return part_stamp_skipped(cpu, rec, cls, &o) ? PART_V_STAMP : (PART_V_STAMP | PART_V_APPEND);
}

/* ---- the widened view's particles -------------------------------------------
 *
 * The routine's window is the 4:3 frame, so in a view wider than 4:3 (a Hor+
 * factor above 1 or a --fov scale below 1, ngte_view.extended) a record whose
 * primitive lies in a revealed side band, or under a --fov scale below 1 in a
 * revealed band above or below, is neither stamped nor appended. Such a
 * record, one the routine's verdict passes on part_reveal_win and fails on
 * PART_WIN_GAME, is a candidate: it
 * becomes a synthetic item built from the record and the type table as the
 * routine builds its primitive, appended after the captured items and fed and
 * rebased as a captured one. It is admitted unless every built vertex lies
 * left of the view's screen-X bound xlo or every one at or right of xhi.
 * Under extended LOD (openpete_no_lod) the same path reveals a record the
 * routine drops for its depth alone (SZ3 >= 0x2000, a class-4 corner sum
 * >= 0x8000): the window's depth ceiling is lifted, every class is built as
 * the routine builds it near, and its colour words pass unchanged, since the
 * routine applies no depth cue to a particle's colour. */

/* OPENPETE_PART_WIDE_CENSUS=LO-HI: per walk inside the VBL range, `cand`
 * candidates, `admit` appended, `deep` the appended ones the routine's depth
 * ceiling alone rejects (extended LOD), `cull` built and outside the view, `mism`
 * records whose verdict at (0, 512) disagrees with the guest's stamp, `recs`
 * records the verdict covered, `syn` paired primitives rebuilt by
 * part_synth_item (per class in the total) and `syn_bad` the rebuilds whose
 * words differ from the captured primitive's; totals print at exit. A walk after the
 * routine stopped at the polybuf's end counts no mismatch (the records it
 * never reached keep the previous call's stamp) and reveals nothing. */
typedef struct { unsigned long cand, admit, deep, cull, mism, recs, exh, syn, syn_bad, syn_c[8]; } part_wide_tally;
static part_wide_tally s_wide, s_wide_total;
static int s_wide_census_walk;   /* the current walk runs the census */
static int s_syn_n;   /* synthetic items appended past s_item_n by the current walk */

/** @brief Fills a paired or synthetic item's built vertices, projected points, view-Z and depth from its record words.
 *
 * @param cpu Reads the sine table.
 * @param v The tick's view.
 * @param it The item; cls and the position words set. */
static void part_item_fill(CPUState* cpu, const ngte_view* v, part_item* it) {
    part_state st;
    part_state_tick(it, &st);
    it->nb = (uint8_t)part_build(cpu, v, it, &st, it->bx, it->by);
    double p[3];
    ngte_proj o;
    part_rec_point(it, 0, NULL, p);
    part_project(v, p, &o);
    it->psx = o.sx;
    it->psy = o.sy;
    it->vz  = o.m3;
    it->depth = openpete_native_scene_viewz(openpete_worldvz_scaled((float)o.m3, 4.0f));
    if (it->cls != PART_TWO_POINT_CLASS) return;
    ngte_proj o2;
    part_rec_point(it, 1, NULL, p);
    part_project(v, p, &o2);
    it->psx2 = o2.sx;
    it->psy2 = o2.sy;
}

/** @brief Writes the words of the POLY_FT4 a billboard class appends, its corners given.
 *
 * Word 1 is the record's colour word (+0xC). e4 and e8 are the words at +4 and
 * +8 of the type's frame entry; the routine stores e4 as UV0 and CLUT, e8 as
 * UV3 and the page with e4's V, and (e4 & 0xFF) | (e8 & 0xFF00) as UV2 (Spyro
 * 2 partsdraw.s:449-459, :600-611).
 * @param col The record's colour word.
 * @param e4 The frame entry's word at +4.
 * @param e8 The frame entry's word at +8.
 * @param xy The four packed corner words.
 * @param w Receives the ten words. */
static void part_ft4_words(uint32_t col, uint32_t e4, uint32_t e8, const uint32_t xy[4],
                           uint32_t w[10]) {
    w[0] = 0x09000000u;
    w[1] = col;
    w[2] = xy[0]; w[3] = e4;
    w[4] = xy[1]; w[5] = (e8 & ~0xFF00u) | (e4 & 0xFF00u);
    w[6] = xy[2]; w[7] = (e4 & 0xFFu) | (e8 & 0xFF00u);
    w[8] = xy[3]; w[9] = e8;
}

/** @brief Builds the primitive the routine would append for a record as a synthetic item.
 *
 * The vertices come from part_int_verts under `g`; a line class takes its
 * colour words from the record (class 0: +0xC, a LINE_F2 of four words,
 * partsdraw.s:726-750; class 1: +0x10 and +0x14, a LINE_G2 of five,
 * :668-692); a billboard class takes its texture words from the type table
 * entry frame * 8 of its type's list (part_ft4_words). The item then carries
 * the record as a paired one does (part_item_fill).
 * @param cpu Reads the record, the type table and the sine table.
 * @param v The tick's view.
 * @param g The GTE state the routine holds at the record.
 * @param rec The record's guest address.
 * @param cls The record's class, below fixed_layer_class.
 * @param it Receives the item.
 * @return 1 when built, 0 when the type has no frame list. */
static int part_synth_item(CPUState* cpu, const ngte_view* v, const part_gte* g,
                           uint32_t rec, int cls, part_item* it) {
    const uint32_t pos = game_layout.particles.pos_off;
    memset(it, 0, sizeof *it);
    it->cls = cls;
    it->rec = rec;
    it->w0 = cpu->read_word(rec + pos);
    it->w1 = cpu->read_word(rec + pos + 4u);
    it->w2 = cpu->read_word(rec + pos + 8u);
    ngte_rtps_out o;
    part_gte_rtps(g, v, (int32_t)(it->w0 & 0xFFFFu), (int32_t)(it->w0 >> 16),
                  (int32_t)(it->w1 & 0xFFFFu), &o);
    uint32_t xy[4], w[10];
    const int nv = part_int_verts(cpu, v, g, it, &o, xy);
    int words = 10;
    if (cls == PART_DOT_CLASS) {
        w[0] = 0x03000000u; w[1] = cpu->read_word(rec + 0x0Cu); w[2] = xy[0]; w[3] = xy[1];
        words = 4;
    } else if (cls == PART_TWO_POINT_CLASS) {
        w[0] = 0x04000000u; w[1] = cpu->read_word(rec + 0x10u); w[2] = xy[0];
        w[3] = cpu->read_word(rec + 0x14u); w[4] = xy[1];
        words = 5;
    } else {
        const uint32_t sel = cpu->read_half(rec + game_layout.particles.sel_off);
        const uint32_t list = game_layout.particles.type_table
            ? cpu->read_word(game_layout.particles.type_table + (sel & 0xFFu) * 4u) : 0u;
        if (list < 0x80000000u || list >= 0x80800000u || nv != 4) return 0;
        const uint32_t e = list + (uint32_t)cpu->read_byte(rec + 0x10u) * 8u;
        part_ft4_words(cpu->read_word(rec + 0x0Cu), cpu->read_word(e + 4u),
                       cpu->read_word(e + 8u), xy, w);
    }
    ngte_prim_decode(w, words, 10, &it->p);
    it->owner = player_hide_particle_owner(cpu, rec);
    part_item_fill(cpu, v, it);
    return 1;
}

/** @brief Whether some built vertex of a synthetic item lies inside the view's screen-X bounds.
 *
 * @param it The item; bx and nb filled.
 * @param v The view; supplies xlo and xhi.
 * @return 1 unless every vertex lies left of xlo or every one at or right of xhi. */
static int part_in_view(const part_item* it, const ngte_view* v) {
    if (it->nb == 0) return 0;
    float lo = it->bx[0], hi = it->bx[0];
    for (int k = 1; k < it->nb; k++) {
        if (it->bx[k] < lo) lo = it->bx[k];
        if (it->bx[k] > hi) hi = it->bx[k];
    }
    return !(hi < v->xlo || lo >= v->xhi);
}

/** @brief Appends the synthetic item of an unstamped record a widened view or extended LOD shows.
 *
 * A candidate is a record the verdict fails on PART_WIN_GAME and passes,
 * appending, on part_reveal_win. It is built by part_synth_item into the slot past
 * the captured items and the earlier synthetic ones, and kept when
 * part_in_view admits it. Tallies s_wide; a census walk also counts an
 * admitted record that fails on the same window at the routine's depth
 * ceiling (s_wide.deep).
 * @param cpu Reads the record and the tables.
 * @param v The tick's view.
 * @param g The GTE state the routine holds at the record.
 * @param rec The record's guest address.
 * @param cls The record's class, below fixed_layer_class. */
static void part_reveal_record(CPUState* cpu, const ngte_view* v, const part_gte* g,
                               uint32_t rec, int cls) {
    if (part_record_verdict(cpu, v, g, rec, cls, &PART_WIN_GAME)) return;
    const part_win wide = part_reveal_win(v);
    if (!(part_record_verdict(cpu, v, g, rec, cls, &wide) & PART_V_APPEND)) return;
    s_wide.cand++;
    const int slot = s_item_n + s_syn_n;
    if (slot >= PART_MAX_PRIMS) return;
    memset(&s_reb[slot], 0, sizeof s_reb[0]);
    if (!part_synth_item(cpu, v, g, rec, cls, &s_item[slot]) || !part_in_view(&s_item[slot], v)) {
        s_wide.cull++;
        return;
    }
    s_syn_n++;
    s_wide.admit++;
    if (s_wide_census_walk && wide.zmax != PART_WIN_ZMAX) {
        part_win shallow = wide;
        shallow.zmax = PART_WIN_ZMAX;
        if (!(part_record_verdict(cpu, v, g, rec, cls, &shallow) & PART_V_APPEND)) s_wide.deep++;
    }
}

/* ---- the pool walk ---------------------------------------------------------- */

/* The routine's walk: the next record and the GTE state the routine holds
 * when it reaches it. */
typedef struct {
    uint32_t rec;
    uint32_t stride;
    int      left;     /* records the walk may still visit */
    part_gte g;
} part_walk;

/** @brief Starts a walk at the pool base under the camera rotation and a zero translation (partsdraw.s:30-42).
 *
 * @param cpu Reads the pool pointer.
 * @param v The tick's view; supplies the camera matrix.
 * @param w Receives the walk.
 * @return 0 when the pool pointer is outside RAM. */
static int part_walk_begin(CPUState* cpu, const ngte_view* v, part_walk* w) {
    w->rec = cpu->read_word(game_layout.particles.array_ptr);
    if (w->rec < 0x80000000u || w->rec >= 0x80800000u) return 0;
    w->stride = game_layout.particles.rec_stride;
    w->left = (int)PART_SLOTS_MAX;
    memcpy(w->g.R, v->cam_M, sizeof w->g.R);
    w->g.tr[0] = w->g.tr[1] = w->g.tr[2] = 0;
    return 1;
}

/** @brief Steps the walk to the next active record, skipping inactive slots, as the routine dispatches.
 *
 * @param cpu Reads the selectors.
 * @param w The walk.
 * @param rec Receives the record's guest address.
 * @param cls Receives its class.
 * @return 0 at the pool's end sentinel or after PART_SLOTS_MAX records. */
static int part_walk_next(CPUState* cpu, part_walk* w, uint32_t* rec, int* cls) {
    while (w->left-- > 0) {
        const uint32_t r = w->rec;
        w->rec += w->stride;
        const int sel = (int)(int16_t)(uint16_t)cpu->read_half(r + game_layout.particles.sel_off);
        if (sel < 0) {
            if ((sel >> PART_CLASS_SHIFT) == PART_POOL_END) return 0;
            continue;                                    /* inactive slot */
        }
        *rec = r;
        *cls = (sel >> PART_CLASS_SHIFT) & PART_CLASS_MASK;
        return 1;
    }
    return 0;
}

/** @brief At OPENPETE_PART_VERTEX_DBG, counts a carried record whose skip verdict under the camera state differs; at 2 or more prints it.
 *
 * @param cpu Reads the record.
 * @param v The tick's view.
 * @param g The modelled state.
 * @param it The item holding the record's position words.
 * @param rec The record's guest address.
 * @param cls The record's class.
 * @param skip The skip verdict under `g`.
 * @param go The record point's RTPS under `g`. */
static void part_carry_dbg(CPUState* cpu, const ngte_view* v, const part_gte* g,
                           const part_item* it, uint32_t rec, int cls, int skip,
                           const ngte_rtps_out* go) {
    if (s_vdbg <= 0 || !part_gte_carried(g, v)) return;
    part_gte cam;
    memcpy(cam.R, v->cam_M, sizeof cam.R);
    cam.tr[0] = cam.tr[1] = cam.tr[2] = 0;
    ngte_rtps_out co;
    part_gte_rtps(&cam, v, (int32_t)(it->w0 & 0xFFFFu), (int32_t)(it->w0 >> 16),
                  (int32_t)(it->w1 & 0xFFFFu), &co);
    const int cskip = part_stamp_skipped(cpu, rec, cls, &co);
    s_gte_flips += (unsigned long)(cskip != skip);
    if (s_vdbg >= 2)
        fprintf(stderr, "[part-carry f=%u rec=%08X c=%d skip=%d cam_skip=%d sz3=%d cam_sz3=%d]\n",
                openpete_frame, rec, cls, skip, cskip, go->sz3, co.sz3);
}

/** @brief Census check of part_synth_item: rebuilds a paired record's primitive and compares its words with the captured ones.
 *
 * Every word after the tag is compared, and the tag's length byte; the tag's
 * link is the ordering table's. Tallies s_wide.syn and s_wide.syn_bad and
 * prints a line per difference.
 * @param cpu Reads the record and the tables.
 * @param v The tick's view.
 * @param g The GTE state the routine held at the record.
 * @param it The paired item. */
static void part_synth_check(CPUState* cpu, const ngte_view* v, const part_gte* g,
                             const part_item* it) {
    part_item syn;
    s_wide.syn++;
    s_wide.syn_c[it->cls & 7]++;
    int bad = !part_synth_item(cpu, v, g, it->rec, it->cls, &syn) || syn.p.taglen != it->p.taglen;
    for (int k = 1; !bad && k <= (int)it->p.taglen && k < NGTE_PRIM_RAW_MAX; k++)
        bad = syn.p.raw[k] != it->p.raw[k];
    if (!bad) return;
    s_wide.syn_bad++;
    fprintf(stderr, "[part-wide-syn f=%u rec=%08X c=%d w1=%08X/%08X w2=%08X/%08X w3=%08X/%08X]\n",
            openpete_frame, it->rec, it->cls, syn.p.raw[1], it->p.raw[1], syn.p.raw[2],
            it->p.raw[2], syn.p.raw[3], it->p.raw[3]);
}

/** @brief Pairs a fixed-layer record with the captured primitive `it`: its position words, class, record, owner, built corners and the fixed front depth.
 *
 * The routine appends one primitive for every record of class
 * fixed_layer_class and up, with no stamp and no window test, so the walk
 * order alone aligns them. The shown point (psx, psy) is the projection of
 * the record's view-space translation, and vz its Z.
 * @param cpu Reads the record and the sine table.
 * @param v The tick's view.
 * @param rec The record's guest address.
 * @param cls The record's class, fixed_layer_class or above.
 * @param front The scene depth of the fixed front bucket.
 * @param it The captured item. */
static void part_pair_fixed(CPUState* cpu, const ngte_view* v, uint32_t rec, int cls,
                            float front, part_item* it) {
    it->w0 = cpu->read_word(rec + game_layout.particles.pos_off);
    it->w1 = cpu->read_word(rec + game_layout.particles.pos_off + 4u);
    it->w2 = cpu->read_word(rec + game_layout.particles.pos_off + 8u);
    it->cls = cls;
    it->rec = rec;
    it->owner = player_hide_particle_owner(cpu, rec);
    it->depth = front;
    part_state st;
    part_state_tick(it, &st);
    it->nb = (uint8_t)part_build(cpu, v, it, &st, it->bx, it->by);
    ngte_view c;
    ngte_view_canon(v, &c);
    ngte_proj o;
    ngte_screen_gte(4.0 * (double)(int16_t)(it->w0 & 0xFFFFu), (double)((int32_t)it->w0 >> 14),
                    4.0 * (double)(int16_t)(it->w1 & 0xFFFFu), c.ofx, c.ofy, &c, 0, &o);
    it->psx = o.sx;
    it->psy = o.sy;
    it->vz = o.m3;
}

/** @brief Pairs a stamped record with the captured primitive `it` and advances the modelled GTE state past it.
 *
 * The record emitted a primitive unless it fails its class's test between the
 * stamp and the append (part_stamp_skipped, on the record point's RTPS under
 * `g`). A paired item keeps the record's position words, class, owner, built
 * vertices, projected points and view-Z (part_item_fill). A stamped record
 * past the last captured primitive (`it` NULL) advances the state only.
 * @param cpu Reads the record.
 * @param v The tick's view.
 * @param g The modelled state, advanced past the record.
 * @param rec The record's guest address.
 * @param cls The record's class, below fixed_layer_class.
 * @param it The next captured item, or NULL.
 * @return 1 when `it` was paired. */
static int part_pair_record(CPUState* cpu, const ngte_view* v, part_gte* g,
                            uint32_t rec, int cls, part_item* it) {
    part_item tmp;
    part_item* t = it ? it : &tmp;
    t->w0 = cpu->read_word(rec + game_layout.particles.pos_off);
    t->w1 = cpu->read_word(rec + game_layout.particles.pos_off + 4u);
    t->w2 = cpu->read_word(rec + game_layout.particles.pos_off + 8u);
    ngte_rtps_out go;
    part_gte_rtps(g, v, (int32_t)(t->w0 & 0xFFFFu), (int32_t)(t->w0 >> 16),
                  (int32_t)(t->w1 & 0xFFFFu), &go);
    const int carried = part_gte_carried(g, v);
    s_gte_carried += (unsigned long)carried;
    s_gte_rot_only += (unsigned long)(carried && !g->tr[0] && !g->tr[1] && !g->tr[2]);
    const int skip = part_stamp_skipped(cpu, rec, cls, &go);
    part_carry_dbg(cpu, v, g, t, rec, cls, skip, &go);
    if (skip) s_stamp_skipped++;
    if (skip || !it) {
        part_gte_step(g, cls, skip, &go);
        return 0;
    }
    it->cls = cls;
    it->rec = rec;
    it->owner = player_hide_particle_owner(cpu, rec);
    part_pair_audit(cpu, v, g, it, &go);
    if (s_wide_census_walk) part_synth_check(cpu, v, g, it);
    part_gte_step(g, cls, 0, &go);
    part_item_fill(cpu, v, it);
    return 1;
}

/** @brief Adds one record to the widened-view census: its verdict at (0, 512) against the guest's stamp.
 *
 * @param cpu Reads the record.
 * @param v The tick's view.
 * @param g The GTE state the routine holds at the record.
 * @param rec The record's guest address.
 * @param cls The record's class.
 * @param stamped The guest's stamp byte, nonzero when set. */
static void part_wide_census_record(CPUState* cpu, const ngte_view* v, const part_gte* g,
                                    uint32_t rec, int cls, int stamped) {
    const int vd = part_record_verdict(cpu, v, g, rec, cls, &PART_WIN_GAME);
    s_wide.recs++;
    if (((vd & PART_V_STAMP) != 0) != (stamped != 0)) {
        s_wide.mism++;
        fprintf(stderr, "[part-wide-mism f=%u rec=%08X c=%d stamp=%d verdict=%d]\n",
                openpete_frame, rec, cls, stamped, vd);
    }
}

/** @brief The mean of a captured primitive's screen vertices.
 *
 * A polygon averages its decoded vertices; a line averages its two endpoints
 * from the raw words, where a gouraud line carries a colour word before each.
 * @param p The captured primitive.
 * @param gx Receives the mean X.
 * @param gy Receives the mean Y. */
static void part_item_centroid(const ngte_prim* p, double* gx, double* gy) {
    double sx = 0, sy = 0;
    if (p->nv) {
        for (int k = 0; k < p->nv; k++) { sx += p->x[k]; sy += p->y[k]; }
        *gx = sx / p->nv; *gy = sy / p->nv;
        return;
    }
    const int words = (p->code & 0x10u) ? 3 : 2;
    for (int k = 0; k < 2; k++) {
        const uint32_t xy = p->raw[2 + (uint32_t)k * (uint32_t)(words - 1)];
        sx += (double)(int16_t)(xy & 0xFFFFu);
        sy += (double)(int16_t)(xy >> 16);
    }
    *gx = sx / 2.0; *gy = sy / 2.0;
}

static unsigned long s_part_order, s_part_nearest, s_part_depth1;

/** @brief Gives every captured primitive the pool walk left unpaired the depth of the paired primitive whose centroid lies nearest its own.
 *
 * A primitive is left unpaired when the pool walk runs out of records before
 * the captured primitives, as when the walk and the routine disagree on a
 * stamp-skip verdict. Fixed-layer primitives are not candidates: they stand
 * at the front bucket by class, not by position. The primitive keeps its
 * captured vertices and stays unpaired (class -1). With no paired candidate
 * it keeps the near-guard depth. Counts the paired, the placed and the
 * near-guard primitives into the run's totals. */
static void part_place_unreached(void) {
    for (int i = 0; i < s_item_n; i++) {
        part_item* it = &s_item[i];
        if (it->cls >= 0) { s_part_order++; continue; }
        double gx, gy, best = INFINITY;
        part_item_centroid(&it->p, &gx, &gy);
        for (int j = 0; j < s_item_n; j++) {
            const part_item* o = &s_item[j];
            if (o->cls < 0 || (uint32_t)o->cls >= game_layout.particles.fixed_layer_class) continue;
            double ox, oy;
            part_item_centroid(&o->p, &ox, &oy);
            const double d = (ox - gx) * (ox - gx) + (oy - gy) * (oy - gy);
            if (d < best) { best = d; it->depth = o->depth; }
        }
        if (best < INFINITY) s_part_nearest++; else s_part_depth1++;
    }
}

/** @brief Walks the pool as the routine does: pairs each captured primitive with the record that appended it, and appends the records a widened view or extended LOD reveals.
 *
 * The routine appends in pool order and at most one primitive per record, so
 * walking the pool and the captured list together aligns them. A stamped
 * record goes to part_pair_record; a record of class fixed_layer_class and
 * up links at the fixed front bucket, where the routine stamps nothing and
 * always appends, so it goes to part_pair_fixed. Without `reveal` or
 * `census` the walk ends at the last captured primitive, as the pairing
 * needs nothing past it; with either it runs to the
 * pool's end: an unstamped record goes to part_reveal_record under `reveal`,
 * and every record below fixed_layer_class to part_wide_census_record under
 * `census`. Synthetic items are appended past the captured ones and counted
 * into s_item_n at the end. Does nothing when the pool pointer is outside RAM.
 * @param cpu Reads the pool.
 * @param v The tick's view.
 * @param reveal Nonzero appends the records part_reveal_record reveals.
 * @param census Nonzero runs the census. */
static void part_pair_depths(CPUState* cpu, const ngte_view* v, int reveal, int census) {
    s_stamp_skipped = 0;
    s_syn_n = 0;
    part_walk w;
    if (!part_walk_begin(cpu, v, &w)) return;
    /* The fixed front bucket is nearer than bucket 0, which the near guard in
     * openpete_native_scene_viewz already maps to the smallest depth the
     * pipeline carries. */
    const float front = openpete_native_scene_viewz(openpete_worldvz_full(1.0f));
    const int full = reveal || census;
    s_wide_census_walk = census;
    int i = 0;
    uint32_t rec;
    int cls;
    while ((i < s_item_n || full) && part_walk_next(cpu, &w, &rec, &cls)) {
        if ((uint32_t)cls >= game_layout.particles.fixed_layer_class) {
            part_gte_step(&w.g, cls, 0, NULL);
            if (i < s_item_n) part_pair_fixed(cpu, v, rec, cls, front, &s_item[i++]);
            continue;
        }
        const int stamped = cpu->read_byte(rec + game_layout.particles.drawn_off) != 0;
        if (census) part_wide_census_record(cpu, v, &w.g, rec, cls, stamped);
        if (!stamped) {
            if (reveal) part_reveal_record(cpu, v, &w.g, rec, cls);
            continue;
        }
        i += part_pair_record(cpu, v, &w.g, rec, cls, i < s_item_n ? &s_item[i] : NULL);
    }
    /* A polybuf that filled mid-pool ends the routine early, so fewer
     * primitives than stamped records is normal. A primitive the walk could
     * not reach takes its nearest paired neighbour's depth. */
    part_place_unreached();
    s_item_n += s_syn_n;
    for (int k = 0; k < s_item_n; k++)
        s_item[k].serial = s_item[k].rec ? particles_slot_serial(s_item[k].rec) : 0u;
}

/** @brief Whether the native particle feed runs; OPENPETE_PARTICLES starting with '0' suppresses it alone.
 *
 * The guest draw still runs, so the A/B prices the particles' painted pixels
 * from one binary. The variable is read once and latched.
 * @return 1 when the feed runs, 0 when suppressed. */
static int part_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PARTICLES"); v = !(e && *e && *e == '0'); }
    return v;
}

/* ---- sub-tick rebase ------------------------------------------------------
 *
 * The primitives are the guest's own output, built under the tick's camera
 * from the tick's pool, so a sub-tick present that re-feeds them shows every
 * particle at the pose the tick drew: neither the camera's motion nor the
 * record's own reaches them. The rebase measures each record point's screen
 * displacement as the difference of two projections - the pool position the
 * present shows under the staged camera, against the position the tick drew
 * under the tick's camera - and moves the primitive by it. At the end of the
 * tick's own interval the displaced position and the staged camera are the
 * tick's, so both projections take identical inputs through identical
 * arithmetic and the displacement is exactly zero. */

/* A particle's speed ceiling, in the pool's quarter world units per tick. The
 * table is keyed by slot address, so a slot a new record respawns into inside
 * one tick pairs across two different particles and would lerp as a streak
 * between them. The selector and allocation-serial tests below catch a reuse
 * the allocator made; this catches one in a pair without a serial, at a
 * distance few particles cover in a tick. */
#define PART_REUSE_CEILING 300.0

/* The record offsets of the colour words the routine copies into the
 * primitive unchanged: every class but PART_TWO_POINT_CLASS stores the word at
 * +0xC as the primitive's colour word (Spyro 2 partsdraw.s:164,180 for the
 * fixed layer, :272,291 PART_SPIN_CLASS, :435,452 PART_RECT_CLASS, :579,597
 * PART_ROT_CLASS, :726-728 PART_DOT_CLASS); the LINE_G2 of
 * PART_TWO_POINT_CLASS takes its two endpoint colours from +0x10 and +0x14
 * (:668,676,685-687), +0xC holding that class's second point. */
#define PART_COL_OFF      0x0Cu
#define PART_COL2_OFF_A   0x10u
#define PART_COL2_OFF_B   0x14u

/** @brief The 24-bit colour word a record shows at phase `t` from its previous to its current snapshot, each byte lerped and rounded.
 *
 * @param pv The previous snapshot's word.
 * @param cv The current snapshot's word.
 * @param t The phase, 0 at the previous state and 1 at the current.
 * @return The colour word, bits 0-23. */
static uint32_t part_col_lerp(uint32_t pv, uint32_t cv, double t) {
    uint32_t out = 0u;
    for (int k = 0; k < 3; k++) {
        const double a = (double)((pv >> (8 * k)) & 0xFFu), b = (double)((cv >> (8 * k)) & 0xFFu);
        int v = (int)floor(a + (b - a) * t + 0.5);
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        out |= (uint32_t)v << (8 * k);
    }
    return out;
}

/** @brief Sets the shown colour words of a record of the previous epoch's set that rolled between that set's draw and the newest capture.
 *
 * The set's primitive carries the pair's prev colour words; the present shows
 * them lerped toward curr's by alpha.
 * @param o The record's snapshot pair; its blob holds the record from offset 0.
 * @param cls The record's class.
 * @param alpha The present's phase.
 * @param st Receives the colour words and col_on. */
static void part_state_colour(const openpete_interp_obj_t* o, int cls, double alpha, part_state* st) {
    const uint32_t oa = cls == PART_TWO_POINT_CLASS ? PART_COL2_OFF_A : PART_COL_OFF;
    const uint32_t ob = cls == PART_TWO_POINT_CLASS ? PART_COL2_OFF_B : PART_COL_OFF;
    if ((uint32_t)o->blob_len < ob + 4u) return;
    uint32_t pa, ca, pb, cb;
    memcpy(&pa, o->prev + oa, 4); memcpy(&ca, o->curr + oa, 4);
    memcpy(&pb, o->prev + ob, 4); memcpy(&cb, o->curr + ob, 4);
    st->col[0] = part_col_lerp(pa, ca, alpha);
    st->col[1] = part_col_lerp(pb, cb, alpha);
    st->col_on = 1;
}

/* The verdict of the pairing: the primitive is lerped only on PART_PAIR_LERP,
 * and holds its current pose otherwise. PART_PAIR_CEIL is a PART_PAIR_REUSE
 * decided by PART_REUSE_CEILING alone, kept apart for the census. */
enum { PART_PAIR_NONE = 0, PART_PAIR_LERP = 1, PART_PAIR_REUSE = 2, PART_PAIR_CEIL = 3 };

/** @brief The state the record shows on this present: its displacement from the captured words and its two state bytes, interpolated between the pair.
 *
 * The pair is the record's previous and current snapshots in the interp-object
 * table; the displacement is (prev - curr) · (1 - alpha) in the pool's units.
 * Byte 3 of the second position word lerps along the shortest arc of the
 * 256-step circle as an angle and linearly as a height; byte 2, the size,
 * lerps linearly. A record with no previous snapshot (one that spawned this
 * tick), a pair whose prev was not observed up to curr (cut_feeder_pair_is_tick), or a blob too short
 * for the position words returns PART_PAIR_NONE and the tick's own state
 * (part_state_tick); the primitive then holds the tick's pose in the world
 * with only the camera's motion rebasing it.
 *
 * The table is keyed by slot address, so a slot a new record respawns into
 * pairs two different particles; lerping across that pair draws a line from the
 * dead particle's position to the live one's. Three tests separate the reuse
 * from a continuing particle and return PART_PAIR_REUSE, in order of what they
 * catch: the selector halfword changing catches a reuse of a different class
 * or type; the allocation serial the capture appends after the record
 * (particles_slot_serial) changing catches a reuse that kept both, the
 * allocator having handed the slot out between the two captures; the speed
 * ceiling PART_REUSE_CEILING applies only to a pair whose serial is zero (a
 * blob too short to carry it, a slot not allocated since the pool base was
 * read, or one the free hook cleared and a write outside the allocator then
 * filled). An equal nonzero serial is one allocation, so its record moved
 * however far it moved, and lerps (the Spyro 3 Dino Mines bubbles, type 0x3E,
 * move 0x15E per tick along their path).
 *
 * While the previous epoch's set is replayed (s_item_ref_prev), its captured
 * words are the state that set's draw read: the pair's prev for a slot that
 * rolled after s_item_ref_cut, which shows prev lerped toward curr by alpha
 * on every channel, the colour words included (part_state_colour), and the
 * pair's curr for a slot that did not, which the newest capture also holds and
 * which therefore shows the captured words at every alpha.
 * @param it The paired captured primitive.
 * @param alpha The present's phase between the two ticks.
 * @param st Receives the state shown; the tick's own unless the verdict is
 *        PART_PAIR_LERP.
 * @return PART_PAIR_LERP, PART_PAIR_NONE, PART_PAIR_REUSE, or PART_PAIR_CEIL
 *         for a reuse the speed ceiling alone decided. */
static int part_record_pair(const part_item* it, double alpha, part_state* st) {
    part_state_tick(it, st);
    const uint32_t pos = game_layout.particles.pos_off;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(it->rec);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns)
        return PART_PAIR_NONE;
    if ((uint32_t)o->blob_len < pos + 12u) return PART_PAIR_NONE;
    /* A record of the previous epoch's set whose slot did not roll after that
     * set's draw shows curr, the state the newest capture also holds: it does
     * not move over the interval. */
    if (s_item_ref_prev && o->curr_ns <= s_item_ref_cut) return PART_PAIR_LERP;
    if (!cut_feeder_pair_is_tick("particle", it->rec, o, pos, 6u)) return PART_PAIR_NONE;
    uint16_t psel, csel;
    memcpy(&psel, o->prev + game_layout.particles.sel_off, 2);
    memcpy(&csel, o->curr + game_layout.particles.sel_off, 2);
    if (psel != csel) return PART_PAIR_REUSE;         /* the slot took a new record */
    const uint32_t ser = game_layout.particles.rec_stride;
    uint32_t pser = 0u, cser = 0u;
    if ((uint32_t)o->blob_len >= ser + 4u) {
        memcpy(&pser, o->prev + ser, 4);
        memcpy(&cser, o->curr + ser, 4);
        if (pser != cser) return PART_PAIR_REUSE;     /* allocated between the captures */
    }
    uint16_t ph[3], ch[3];   /* X, Y, Z: the first word's halves, the second's low half */
    memcpy(ph, o->prev + pos, 6);
    memcpy(ch, o->curr + pos, 6);
    /* A record of the previous epoch's set drew the slot's prev state (the slot
     * rolled after that draw, tested above): the displayed state is the same,
     * and the offsets are measured from prev, so they gain curr - prev. */
    const double from_prev = s_item_ref_prev ? 1.0 : 0.0;
    /* The halfwords wrap: a fixed-layer record's are signed view coordinates,
     * and every other class's reach the GTE as an int16 difference from the
     * camera (part_project), so a record crossing 0 or 0xFFFF moved the
     * shorter way round. */
    double d[3];
    for (int k = 0; k < 3; k++) d[k] = (double)(int16_t)(uint16_t)(ph[k] - ch[k]);
    /* An equal nonzero serial on both sides is one allocation: the record
     * continued, however far it moved. */
    if (cser == 0u &&
        d[0]*d[0] + d[1]*d[1] + d[2]*d[2] > PART_REUSE_CEILING * PART_REUSE_CEILING)
        return PART_PAIR_CEIL;
    const double rest = 1.0 - alpha;
    for (int k = 0; k < 3; k++) st->off[k] = d[k] * (rest - from_prev);
    /* Bytes 2 and 3 of the second word: the size (partsdraw.s:495-496 takes
     * (w1 >> 16) & 0xFF) and the angle or height (:497-501 shifts the word
     * right by 23 and masks the halfword index to 0x1FE; :394 shifts it right
     * by 24). Both are absolute, so the shown value lerps from curr. */
    const double pa = (double)o->prev[pos + 7u], ca = (double)o->curr[pos + 7u];
    const double ps = (double)o->prev[pos + 6u], cs = (double)o->curr[pos + 6u];
    double da = pa - ca;
    while (da > 128.0) da -= 256.0;
    while (da < -128.0) da += 256.0;
    st->ang = ca + da * rest;
    st->hgt = ca + (pa - ca) * rest;
    st->size = cs + (ps - cs) * rest;
    if (s_item_ref_prev) part_state_colour(o, it->cls, alpha, st);
    return PART_PAIR_LERP;
}

/* OPENPETE_PART_REBASE_DBG=1: the rebase's verdicts per run, by class. A class
 * that only ever snaps has no pairs in the store; a run whose lerped count is
 * zero rebased nothing and measured nothing. `reuse` is the subset of the
 * snapped whose slot was found holding a different record than the pair's
 * previous blob, which is what a rising reuse count against a steady lerp count
 * separates from a pool that is simply churning; `ceil` is the subset of the
 * reused that PART_REUSE_CEILING alone decided. */
static unsigned long s_reb_stat[4][16];   /* 0 lerped, 1 snapped, 2 reused, 3 ceiling */
static int s_reb_dbg;   /* 2 = one line per rebased primitive */

/** @brief Fills s_reb[i] with the rebase of captured primitive i onto the pose this present shows.
 *
 * A primitive with no record gets an inactive rebase, counted snapped when
 * it has a class. Otherwise part_record_pair gives the state the record
 * shows, part_build rebuilds the vertices at that state under `v`, and the
 * record point's projection there gives the depth (and the displacement the
 * debug lines print); a fixed-layer record keeps the fixed front depth. A
 * pair that is not lerped shows the tick's state, so only the camera's motion
 * moves it. Adds to the s_reb_stat verdict counts, and at
 * OPENPETE_PART_REBASE_DBG 2 or more prints one line.
 * @param cpu Reads the sin/cos table.
 * @param v The view under the staged camera.
 * @param alpha The present's phase between the two ticks.
 * @param i The index into s_item. */
static void part_rebase_item(CPUState* cpu, const ngte_view* v, double alpha, int i) {
    part_item* it = &s_item[i];
    part_rebase* r = &s_reb[i];
    memset(r, 0, sizeof *r);
    if (it->cls < 0) return;                     /* unpaired */
    if (!it->rec) { s_reb_stat[1][it->cls & 0xF]++; return; }   /* held at the tick's pose */
    part_state st;
    const int verdict = part_record_pair(it, alpha, &st);
    r->verdict = (uint8_t)verdict;
    s_reb_stat[verdict == PART_PAIR_LERP ? 0 : 1][it->cls & 0xF]++;
    if (verdict == PART_PAIR_REUSE || verdict == PART_PAIR_CEIL) s_reb_stat[2][it->cls & 0xF]++;
    if (verdict == PART_PAIR_CEIL) s_reb_stat[3][it->cls & 0xF]++;
    if ((uint32_t)it->cls >= game_layout.particles.fixed_layer_class) {
        r->depth = it->depth;                    /* the fixed front bucket */
    } else {
        double p[3];
        ngte_proj o;
        part_rec_point(it, 0, st.off, p);
        part_project(v, p, &o);
        r->dx = (float)(o.sx - it->psx);
        r->dy = (float)(o.sy - it->psy);
        r->depth = openpete_native_scene_viewz(openpete_worldvz_scaled((float)o.m3, 4.0f));
    }
    r->active = 1;
    r->dsz = (st.size != (double)((it->w1 >> 16) & 0xFFu)) ? 1u : 0u;
    r->shown[0] = (double)(it->w0 & 0xFFFFu) + st.off[0];
    r->shown[1] = (double)(it->w0 >> 16) + st.off[1];
    r->shown[2] = (double)(it->w1 & 0xFFFFu) + st.off[2];
    r->shown[3] = st.size;
    r->shown[4] = st.ang;
    r->col_on = (uint8_t)st.col_on;
    r->col[0] = st.col[0];
    r->col[1] = st.col[1];
    r->nb = (uint8_t)part_build(cpu, v, it, &st, r->bx, r->by);
    if (s_reb_dbg >= 2)
        fprintf(stderr, "[part-reb f=%u rec=%08X c=%d v=%d off=(%.2f,%.2f,%.2f) "
                        "ps=(%.2f,%.2f) new=(%.2f,%.2f) d=(%.2f,%.2f) nb=%u\n",
                openpete_frame, it->rec, it->cls, verdict, st.off[0], st.off[1], st.off[2],
                it->psx, it->psy, it->psx + (double)r->dx, it->psy + (double)r->dy,
                (double)r->dx, (double)r->dy, (unsigned)r->nb);
}

/** @brief Prints a view's camera, offsets, H, scale and matrix row to stderr.
 *
 * @param tag The label printed in the line.
 * @param v The view. */
static void part_view_dbg(const char* tag, const ngte_view* v) {
    fprintf(stderr, "[part-view f=%u %s] cam=(%.2f,%.2f,%.2f) ram=(%.2f,%.2f,%.2f) "
                    "interp=%d of=(%.3f,%.3f) H=%.2f/%.2f s=%.4f F=%.4f M0=(%d,%d,%d)\n",
            openpete_frame, tag, v->cam[0], v->cam[1], v->cam[2],
            v->cam_ram[0], v->cam_ram[1], v->cam_ram[2], v->cam_interp,
            v->ofx, v->ofy, v->H_canon, v->H_eff, v->s, v->F,
            v->cam_M[0][0], v->cam_M[0][1], v->cam_M[0][2]);
}

/** @brief Prints the free hook's count and the per-class rebase verdict counts to stderr; registered to run at exit.
 *
 * A class with no lerped and no snapped primitive is omitted. */
static void part_rebase_report(void) {
    OP_ROLE_ASSERT(sim);
    fprintf(stderr, "[part-rebase] free=%lu", s_free_hits);
    for (int c = 0; c < 16; c++)
        if (s_reb_stat[0][c] || s_reb_stat[1][c])
            fprintf(stderr, " c%d:lerp=%lu snap=%lu reuse=%lu ceil=%lu", c,
                    s_reb_stat[0][c], s_reb_stat[1][c], s_reb_stat[2][c], s_reb_stat[3][c]);
    fprintf(stderr, "\n");
}

/** @brief Latches OPENPETE_PART_REBASE_DBG into s_reb_dbg on the first call; nonzero registers the per-class verdict report at exit. */
static void part_dbg_latch(void) {
    static int dbg = -1;
    if (dbg >= 0) return;
    const char* e = getenv("OPENPETE_PART_REBASE_DBG");
    dbg = (e && *e && *e != '0');
    s_reb_dbg = e ? atoi(e) : 0;
    if (dbg) atexit(part_rebase_report);
}

/** @brief Rebases the captured set onto the pose this present shows.
 *
 * The staged camera resolves through the sub-tick view, so building a view
 * here reads the interpolated transform the rest of the pass draws under.
 * At OPENPETE_PART_REBASE_DBG 2 or more (part_dbg_latch) it prints the view
 * and one line per rebased primitive.
 * @param cpu Reads the staged camera and the sin/cos table.
 * @param alpha The present's phase between the two ticks. */
static void part_rebase_all(CPUState* cpu, double alpha) {
    if (s_item_n <= 0) return;
    part_dbg_latch();
    ngte_view v;
    ngte_view_build(cpu, &v);
    if (s_reb_dbg >= 2) part_view_dbg("staged", &v);
    for (int i = 0; i < s_item_n; i++) part_rebase_item(cpu, &v, alpha, i);
}

/* The phase of the present the next feed draws: the rebase's alpha, or -1
 * for the tick's own capture. */
static double s_feed_alpha = -1.0;

/* OPENPETE_PART_VERTEX_DBG: per class, the census of the vertices the feed
 * draws against the vertices part_build builds from the record's state.
 * `cal` is the captured primitive's integer vertex minus the build at the
 * tick's state, per axis (the routine's truncation; `in` counts vertices
 * whose both axes lie in (-1, 0], `far` those more than 4 px from the
 * build); `can` and `sub` are the distance from each
 * drawn vertex to the build at the state the present shows, on the canonical
 * and the sub-tick feeds; `jit` is the change of the canonical vertex error
 * of one record between consecutive captured sets, the step a vertex makes
 * at the tick boundary that its record's motion does not; `miss` counts the
 * paired primitives part_build built nothing for. A count of zero measured
 * nothing. The pairing audit (part_pair_audit) prints beside it. */
typedef struct { unsigned long n; double sum, max; } part_err_acc;
typedef struct { unsigned long n, in, far; double sum[2], lo[2], hi[2]; } part_cal_acc;
static part_cal_acc s_vcal[16];
static part_err_acc s_vplc[2][16];
static part_err_acc s_vjit[16];
static unsigned long s_vmiss[16];
static int s_vdbg = -1;

/** @brief Prints the OPENPETE_PART_VERTEX_DBG census to stderr, one line per class; registered to run at exit. */
static void part_vertex_report(void) {
    OP_ROLE_ASSERT(sim);
    for (int c = 0; c < 16; c++) {
        const part_cal_acc* k = &s_vcal[c];
        if (!k->n && !s_vplc[0][c].n && !s_vplc[1][c].n && !s_vmiss[c]) continue;
        const double n = k->n ? (double)k->n : 1.0;
        fprintf(stderr, "[part-vertex c%d] cal n=%lu mean=(%.4f,%.4f) lo=(%.4f,%.4f) hi=(%.4f,%.4f) "
                        "in=%.4f far=%lu", c, k->n, k->sum[0] / n, k->sum[1] / n, k->lo[0], k->lo[1],
                k->hi[0], k->hi[1], (double)k->in / n, k->far);
        const char* tag[3] = { "can", "sub", "jit" };
        const part_err_acc* e[3] = { &s_vplc[0][c], &s_vplc[1][c], &s_vjit[c] };
        for (int j = 0; j < 3; j++)
            fprintf(stderr, " %s n=%lu mean=%.4f max=%.4f", tag[j], e[j]->n,
                    e[j]->n ? e[j]->sum / (double)e[j]->n : 0.0, e[j]->max);
        fprintf(stderr, " miss=%lu\n", s_vmiss[c]);
    }
    fprintf(stderr, "[part-audit] carry=%d skips=%lu carried=%lu rot_only=%lu flips=%lu",
            part_gte_carry_on(), s_gte_skips, s_gte_carried, s_gte_rot_only, s_gte_flips);
    for (int c = 0; c < 16; c++)
        if (s_aud[0][c])
            fprintf(stderr, " c%d:n=%lu id=%lu pos=%lu car=%lu", c, s_aud[0][c], s_aud[1][c],
                    s_aud[2][c], s_aud[3][c]);
    fprintf(stderr, "\n");
}

/** @brief Whether OPENPETE_PART_VERTEX_DBG is set; latched on the first call, which registers part_vertex_report at exit.
 *
 * @return 1 when the vertex census runs. */
static int part_vdbg_on(void) {
    if (s_vdbg < 0) {
        const char* e = getenv("OPENPETE_PART_VERTEX_DBG");
        s_vdbg = (e && *e && *e != '0') ? (atoi(e) > 1 ? atoi(e) : 1) : 0;
        if (s_vdbg) atexit(part_vertex_report);
    }
    return s_vdbg;
}

/** @brief Adds one distance to an error accumulator.
 *
 * @param a The accumulator.
 * @param d The distance. */
static void part_err_add(part_err_acc* a, double d) {
    a->n++;
    a->sum += d;
    if (d > a->max) a->max = d;
}

/** @brief A captured primitive's integer vertex k: a polygon's corner (a triangle repeats its last for k 3), or a line's endpoint from the raw words.
 *
 * @param p The captured primitive.
 * @param k The vertex index.
 * @param x Receives the screen X.
 * @param y Receives the screen Y. */
static void part_raw_vertex(const ngte_prim* p, int k, double* x, double* y) {
    if (p->nv) {
        const int j = k < p->nv ? k : p->nv - 1;
        *x = p->x[j];
        *y = p->y[j];
        return;
    }
    const int words = (p->code & 0x10u) ? 3 : 2;
    const uint32_t xy = p->raw[2 + (uint32_t)k * (uint32_t)(words - 1)];
    *x = (double)(int16_t)(xy & 0xFFFFu);
    *y = (double)(int16_t)(xy >> 16);
}

/** @brief Adds a paired primitive's calibration to the vertex census: its integer vertices minus the build at the tick's state.
 *
 * @param it The paired captured primitive. */
static void part_vertex_calib(const part_item* it) {
    if (!part_vdbg_on() || !it->rec) return;
    const int c = it->cls & 0xF;
    if (!it->nb) { s_vmiss[c]++; return; }
    part_cal_acc* a = &s_vcal[c];
    for (int k = 0; k < it->nb; k++) {
        double rx, ry;
        part_raw_vertex(&it->p, k, &rx, &ry);
        const double d[2] = { rx - it->bx[k], ry - it->by[k] };
        for (int j = 0; j < 2; j++) {
            if (!a->n || d[j] < a->lo[j]) a->lo[j] = d[j];
            if (!a->n || d[j] > a->hi[j]) a->hi[j] = d[j];
            a->sum[j] += d[j];
        }
        if (d[0] > -1.0 && d[0] <= 0.0 && d[1] > -1.0 && d[1] <= 0.0) a->in++;
        if (d[0] * d[0] + d[1] * d[1] > 16.0) a->far++;
        a->n++;
    }
}

/** @brief Adds the vertices the feed draws for captured primitive i to the vertex census: each one's distance from the build at the shown state.
 *
 * The shown state's build is the rebase's on a sub-tick feed and the
 * capture's on a canonical one; a canonical feed also keeps each vertex's
 * error in the item for the tick-boundary step (part_vertex_jitter).
 * @param i The index into s_item.
 * @param x The drawn screen X.
 * @param y The drawn screen Y.
 * @param n The vertex count. */
static void part_vertex_place(int i, const float* x, const float* y, int n) {
    if (!part_vdbg_on()) return;
    part_item* it = &s_item[i];
    const part_rebase* r = &s_reb[i];
    if (!it->rec) return;
    const int sub = s_feed_alpha >= 0.0;
    const float* bx = sub ? r->bx : it->bx;
    const float* by = sub ? r->by : it->by;
    const int nb = sub ? r->nb : it->nb;
    if (nb != n) return;
    for (int k = 0; k < n; k++) {
        const double ex = (double)x[k] - bx[k], ey = (double)y[k] - by[k];
        part_err_add(&s_vplc[sub][it->cls & 0xF], sqrt(ex * ex + ey * ey));
        if (!sub) { it->verr[k][0] = (float)ex; it->verr[k][1] = (float)ey; }
    }
    if (!sub) it->verr_n = (uint8_t)n;
}

/** @brief Adds the tick-boundary step to the vertex census: for each record in both the current and the previous captured set, the change of each vertex's canonical error.
 *
 * Both sets are in pool order, so one merge walk on the record address pairs
 * them. */
static void part_vertex_jitter(void) {
    if (!part_vdbg_on()) return;
    const part_item* pv = s_item_prev.item;
    int j = 0;
    for (int i = 0; i < s_item_n; i++) {
        const part_item* it = &s_item[i];
        if (!it->rec || !it->verr_n) continue;
        while (j < s_item_prev.n && (!pv[j].rec || pv[j].rec < it->rec)) j++;
        if (j >= s_item_prev.n) break;
        if (pv[j].rec != it->rec || pv[j].cls != it->cls || pv[j].verr_n != it->verr_n) continue;
        for (int k = 0; k < it->verr_n; k++) {
            const double dx = (double)it->verr[k][0] - pv[j].verr[k][0];
            const double dy = (double)it->verr[k][1] - pv[j].verr[k][1];
            part_err_add(&s_vjit[it->cls & 0xF], sqrt(dx * dx + dy * dy));
        }
    }
}

/** @brief The vertices the feed draws a captured primitive at: n corners of a polygon or the two endpoints of a line.
 *
 * In order of precedence: the vertices the rebase built at the shown state,
 * the vertices built at capture, or the primitive's own integer vertices (an
 * unpaired primitive, or one whose shape is not its class's). A triangle
 * fills corner 3 with its last corner.
 * @param it The captured primitive.
 * @param r Its rebase.
 * @param n The vertex count: 4 for a polygon, 2 for a line.
 * @param x Receives the screen X.
 * @param y Receives the screen Y.
 * @return 1 when the vertices are a build's GTE float projections, 0 when
 *         they are the primitive's integer pixel coordinates. */
static int part_drawn_vertices(const part_item* it, const part_rebase* r, int n,
                               float x[4], float y[4]) {
    const float* bx = (r->active && r->nb == n) ? r->bx : (it->nb == n ? it->bx : NULL);
    const float* by = (r->active && r->nb == n) ? r->by : it->by;
    for (int k = 0; k < n; k++) {
        if (bx) { x[k] = bx[k]; y[k] = by[k]; continue; }
        double rx, ry;
        part_raw_vertex(&it->p, k, &rx, &ry);
        x[k] = (float)rx;
        y[k] = (float)ry;
    }
    return bx != NULL;
}

/* OPENPETE_LATE_TRACE=<lo>:<hi>: one line per fed primitive on every present
 * whose feed key lies in [lo, hi] (tests/harness/check_late_switch_subtick.sh).
 * Read once. */
static unsigned s_late_lo = 1u, s_late_hi;

/** @brief Whether the OPENPETE_LATE_TRACE window covers the present in flight; parses the variable on the first call.
 *
 * @return 1 when a line prints for this present, else 0. */
static int part_late_trace_on(void) {
    static int parsed;
    if (!parsed) {
        parsed = 1;
        const char* e = getenv("OPENPETE_LATE_TRACE");
        unsigned lo = 0, hi = 0;
        if (e && sscanf(e, "%u:%u", &lo, &hi) == 2 && lo <= hi) { s_late_lo = lo; s_late_hi = hi; }
    }
    const unsigned k = openpete_feed_key_now();
    return s_late_lo <= s_late_hi && k >= s_late_lo && k <= s_late_hi;
}

/** @brief Prints the state one fed primitive shows, under OPENPETE_LATE_TRACE.
 *
 * `[late-part vbl=<v> leg=canon|inter a=<alpha> ep=<set> new=<newest>]
 * id=<slot>:<serial> cls=<c> st=<x>,<y>,<z>,<size>,<angle> c=<rgb0>,<rgb1>
 * v=<x>,<y>;...`: the epoch of the set drawn and of the newest captured set,
 * the record's pool slot and allocation serial (slot -1 for a primitive
 * paired with no record), the record point's halfwords, size and angle bytes
 * as shown (the captured words on a canonical feed or an inactive rebase), the
 * colour words fed and the fed vertices.
 * @param it The captured primitive.
 * @param r Its rebase.
 * @param x The fed vertex X.
 * @param y The fed vertex Y.
 * @param n The vertex count.
 * @param c0 The first fed colour word.
 * @param c1 The last fed colour word. */
static void part_late_trace(const part_item* it, const part_rebase* r, const float* x,
                            const float* y, int n, uint32_t c0, uint32_t c1) {
    if (!part_late_trace_on()) return;
    double st[5];
    if (r->active) memcpy(st, r->shown, sizeof st);
    else {
        st[0] = (double)(it->w0 & 0xFFFFu); st[1] = (double)(it->w0 >> 16);
        st[2] = (double)(it->w1 & 0xFFFFu); st[3] = (double)((it->w1 >> 16) & 0xFFu);
        st[4] = (double)((it->w1 >> 24) & 0xFFu);
    }
    const int inter = openpete_subtick_view_active();
    const uint32_t newest = (s_item_held == CUT_HELD_PREV) ? s_item_prev.epoch : s_item_epoch;
    char vb[160];
    int o = 0;
    for (int k = 0; k < n && o < (int)sizeof vb - 24; k++)
        o += snprintf(vb + o, sizeof vb - (size_t)o, "%s%.3f,%.3f", k ? ";" : "", (double)x[k], (double)y[k]);
    fprintf(stderr, "[late-part vbl=%u leg=%s a=%.4f ep=%u new=%u] id=%d:%u cls=%d "
                    "st=%.3f,%.3f,%.3f,%.3f,%.3f c=%06x,%06x v=%s\n",
            openpete_feed_key_now(), inter ? "inter" : "canon", inter ? s_feed_alpha : 1.0,
            s_item_epoch, newest, it->rec ? particles_slot(it->rec) : -1, it->serial, it->cls,
            st[0], st[1], st[2], st[3], st[4], c0 & 0xFFFFFFu, c1 & 0xFFFFFFu, vb);
}

/** @brief The four corner colours a polygon feeds: the primitive's, or the record's shown colour word on every corner when the rebase lerps it.
 *
 * A triangle fills the unread corner 3 with its last corner's colour.
 * @param it The captured primitive.
 * @param r Its rebase.
 * @param c Receives the colour words. */
static void part_quad_colours(const part_item* it, const part_rebase* r, uint32_t c[4]) {
    const ngte_prim* p = &it->p;
    if (r->active && r->col_on) { c[0] = c[1] = c[2] = c[3] = r->col[0]; return; }
    const int last = p->nv - 1;
    c[0] = p->rgb[0]; c[1] = p->rgb[1]; c[2] = p->rgb[2]; c[3] = p->rgb[last];
}

/** @brief Feeds a LINE_F2 or LINE_G2 primitive as the quad covering the pixels the GPU draws for it.
 *
 * The endpoints are part_drawn_vertices'; the quad is ngte_line_quad's, with
 * centre offset 0 for a build's float endpoints and 0.5 for the primitive's
 * integer ones. A class-0 dot (SXY to SXY + 1) covers two pixels. A gouraud
 * line colours each endpoint from its own word; the semi-transparency bit
 * selects blend code 1.
 * @param it The captured primitive.
 * @param r Its rebase.
 * @param depth The scene depth to feed at.
 * @param gouraud Nonzero for LINE_G2. */
static void part_feed_line(const part_item* it, const part_rebase* r,
                           float depth, int gouraud) OP_REQUIRES_SHARED(sim) {
    const ngte_prim* p = &it->p;
    const uint32_t w1 = p->raw[1];
    const int stp = (int)((w1 >> 24) & 0x02u);
    const int col = r->active && r->col_on;
    const uint32_t rgb0 = col ? r->col[0] : (w1 & 0xFFFFFFu);
    const uint32_t rgb1 = gouraud ? (col ? r->col[1] : (p->raw[3] & 0xFFFFFFu)) : rgb0;
    float x[4], y[4], qx[4], qy[4];
    const int built = part_drawn_vertices(it, r, 2, x, y);
    part_vertex_place((int)(it - s_item), x, y, 2);
    part_late_trace(it, r, x, y, 2, rgb0, rgb1);
    ngte_line_quad(x[0], y[0], x[1], y[1], built ? 0.0f : 0.5f, qx, qy);
    static const float zu[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const uint32_t cc[4] = { rgb0, rgb1, rgb0, rgb1 };
    s_fed_tris += (uint32_t)ngte_feed_flat_quad(qx, qy, zu, zu, cc, depth, NULL, stp ? 1 : 0, 4);
}

/** @brief At OPENPETE_PART_REBASE_DBG 3 or more, prints one line per fed quad of a paired record: its drawn centroid and the point the record shows.
 *
 * The shown point is the record point's projection under the present's
 * camera (the tick's own projection on a canonical feed); the drawn centroid
 * minus it is the quad's placement error, whose change between consecutive
 * presents is the motion the record does not make. The line also carries the
 * centroid of the guest's integer primitive and the record point's projection
 * under the tick's camera, whose difference calibrates the float corners
 * against the routine's own.
 * @param it The captured primitive.
 * @param r Its rebase.
 * @param x The four corner X the feed draws.
 * @param y The four corner Y the feed draws. */
static void part_track_dbg(const part_item* it, const part_rebase* r,
                           const float x[4], const float y[4]) {
    if (s_reb_dbg < 3 || !it->rec) return;
    const double gx = (x[0] + x[1] + x[2] + x[3]) / 4.0, gy = (y[0] + y[1] + y[2] + y[3]) / 4.0;
    const double ax = it->psx + (r->active ? r->dx : 0.0f);
    const double ay = it->psy + (r->active ? r->dy : 0.0f);
    const ngte_prim* p = &it->p;
    const int last = p->nv - 1;
    const double px = (p->x[0] + p->x[1] + p->x[2] + p->x[last]) / 4.0;
    const double py = (p->y[0] + p->y[1] + p->y[2] + p->y[last]) / 4.0;
    fprintf(stderr, "[part-track f=%u a=%.3f rec=%08X c=%d drawn=(%.3f,%.3f) shown=(%.3f,%.3f) "
                    "guest=(%.3f,%.3f) tick=(%.3f,%.3f)]\n",
            openpete_frame, s_feed_alpha, it->rec, it->cls, gx, gy, ax, ay, px, py,
            it->psx, it->psy);
}

/** @brief Feeds every captured primitive to the native scene: polygons as quads, lines as thin quads.
 *
 * Does nothing when the feed is suppressed or the captured set is empty.
 * Begins the scene at openpete_frame and takes each primitive's depth from its
 * rebase when active, else from its pairing. A textured polygon's blend comes
 * from its ABR when semi-transparent (rate 3 drawn additive), an untextured
 * one's is 1 when semi-transparent; corners pass through the rebase. A code in
 * the 0x40 or 0x50 line ranges goes to part_feed_line; any other code is
 * ignored. While player_hide_active holds, a primitive whose record the
 * player's or the companion's update spawned is not fed. Clears the scene
 * tag on exit. */
static void part_feed_native(void) OP_REQUIRES(sim) {
    if (!part_feed_enabled()) return;
    if (s_item_n <= 0) return;
    const int hide_player = player_hide_active();
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_item_n; i++) {
        const part_item* it = &s_item[i];
        const part_rebase* r = &s_reb[i];
        const ngte_prim* p = &it->p;
        player_hide_count_data(it->owner == 1 ? PH_CAT_PLAYER_PART
                               : it->owner == 2 ? PH_CAT_SPARX_PART : PH_CAT_OTHER_PART,
                               it->owner && hide_player, p, sizeof *p);
        if (it->owner && hide_player) continue;
        const float depth = r->active ? r->depth : it->depth;
        if (p->nv == 4 || p->nv == 3) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_QUAD, (uint32_t)p->code, 0));
            ngte_tpage tp;
            const int blend = ngte_prim_tpage_blend(p, &tp);
            /* A triangle fills the unread corner 3 with its last corner. */
            const int last = p->nv - 1;
            float x[4], y[4];
            part_drawn_vertices(it, r, 4, x, y);
            part_track_dbg(it, r, x, y);
            part_vertex_place(i, x, y, 4);
            const float u[4] = { (float)p->u[0], (float)p->u[1], (float)p->u[2], (float)p->u[last] };
            const float vt[4] = { (float)p->v[0], (float)p->v[1], (float)p->v[2], (float)p->v[last] };
            uint32_t c[4];
            part_quad_colours(it, r, c);
            part_late_trace(it, r, x, y, 4, c[0], c[3]);
            s_fed_tris += (uint32_t)ngte_feed_flat_quad(x, y, u, vt, c, depth,
                                                        p->textured ? &tp : NULL, blend, p->nv);
        } else if ((p->code & 0xF8u) == 0x40u || (p->code & 0xF8u) == 0x50u) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_LINE, (uint32_t)p->code, 0));
            part_feed_line(it, r, depth, (p->code & 0x10u) != 0);
        }
    }
    openpete_native_scene_set_tag(0);
}

/** @brief The screen point a correctly paired primitive's centroid sits at: the record's projected point, or the midpoint of the two-point class's two.
 *
 * The routine offsets a billboard's corners around the record point, and
 * draws a two-point record as the line between its two projected points
 * (partsdraw.s:683-684), whose centroid is their midpoint.
 * @param it The paired item.
 * @param ax Receives the X.
 * @param ay Receives the Y. */
static void part_item_anchor(const part_item* it, double* ax, double* ay) {
    const int two = it->cls == PART_TWO_POINT_CLASS;
    *ax = two ? (it->psx + it->psx2) / 2.0 : it->psx;
    *ay = two ? (it->psy + it->psy2) / 2.0 : it->psy;
}

/** @brief Prints the particle census line when openpete_frame lies in the OPENPETE_PARTICLE_CENSUS range.
 *
 * The range is LO-HI or a single openpete_frame value. Per call it prints the
 * primitives the routine appended by GPU code, the native triangles this
 * feeder emitted from them, and per class the calibration the depth pairing
 * rests on: the mean and worst Manhattan distance between the point each
 * paired record anchors (part_item_anchor) and the centroid of the primitive
 * the pairing gave it, followed by the unpaired count, the stamped records
 * the routine appended nothing for, the fixed-layer primitives with the
 * count of them paired to a record, and the run's totals of captured
 * primitives paired, placed at a neighbour's depth (part_place_unreached) and
 * left at the near guard. A correct pairing (and a correct camera
 * matrix) keeps the distance small and a wrong one scatters it across the
 * screen. A capture whose fed count is zero measured nothing.
 * @param start Guest address of the polybuf interval's start.
 * @param end Guest address one past its end.
 * @param fed_before s_fed_tris before this capture's feed. */
static void part_census(uint32_t start, uint32_t end, uint32_t fed_before) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_PARTICLE_CENSUS", openpete_frame)) return;
    fprintf(stderr, "[part-census f=%u] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, start, end, end - start, s_item_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_item_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    fprintf(stderr, " fed_tris=%u pairs:", s_fed_tris - fed_before);
    for (int cl = 0; cl < 16; cl++) {
        int n = 0; double sum = 0, worst = 0;
        for (int i = 0; i < s_item_n; i++) {
            const part_item* it = &s_item[i];
            if (it->cls != cl || (uint32_t)cl >= game_layout.particles.fixed_layer_class) continue;
            double gx, gy, ax, ay;
            part_item_centroid(&it->p, &gx, &gy);
            part_item_anchor(it, &ax, &ay);
            const double d = fabs(gx - ax) + fabs(gy - ay);
            sum += d; if (d > worst) worst = d; n++;
        }
        if (n) fprintf(stderr, " c%d:n=%d mean=%.2f worst=%.1f", cl, n, sum / n, worst);
    }
    int unpaired = 0, fixed = 0, fixed_rec = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (s_item[i].cls < 0) unpaired++;
        if ((uint32_t)s_item[i].cls >= game_layout.particles.fixed_layer_class && s_item[i].cls >= 0) {
            fixed++;
            fixed_rec += s_item[i].rec != 0u;
        }
    }
    fprintf(stderr, " unpaired=%d stamp_skipped=%d fixed=%d fixed_rec=%d"
                    " total paired=%lu nearest=%lu depth1=%lu\n", unpaired,
            s_stamp_skipped, fixed, fixed_rec, s_part_order, s_part_nearest, s_part_depth1);
}

/** @brief The distance from the record's own projected point to the primitive's furthest vertex, in pixels.
 *
 * A polygon measures every vertex; a line measures its two endpoints from the
 * raw words, where a gouraud line carries a colour word between them.
 * @param it The captured primitive.
 * @return The largest vertex distance. */
static double part_item_extent(const part_item* it) {
    const ngte_prim* p = &it->p;
    double worst = 0;
    if (p->nv) {
        for (int k = 0; k < p->nv; k++) {
            const double dx = p->x[k] - it->psx, dy = p->y[k] - it->psy;
            const double d = sqrt(dx * dx + dy * dy);
            if (d > worst) worst = d;
        }
        return worst;
    }
    const int words = (p->code & 0x10u) ? 3 : 2;
    for (int k = 0; k < 2; k++) {
        const uint32_t xy = p->raw[2 + (uint32_t)k * (uint32_t)(words - 1)];
        const double dx = (double)(int16_t)(xy & 0xFFFFu) - it->psx;
        const double dy = (double)(int16_t)(xy >> 16) - it->psy;
        const double d = sqrt(dx * dx + dy * dy);
        if (d > worst) worst = d;
    }
    return worst;
}

/** @brief OPENPETE_PART_EXTENT_CENSUS: per present, per class, the decomposition of a primitive's painted extent into the terms a sub-tick present could carry.
 *
 * `ext` is the mean part_item_extent, which is the extent the routine built
 * from the record's state under the tick's camera; `vz` is the record point's
 * mean view-Z, whose reciprocal scales every extent; `dsz` counts the records whose size byte differs between the pair's
 * two states. A step in `ext` between two canonical presents at a steady `n`
 * is the primitive's own extent moving; a step in `n` at a steady `ext` is the
 * pool's population, which no rebase of the captured set can carry. The
 * variable is latched on the first call.
 * @param alpha The present's phase, printed; the tick's own capture passes -1. */
static void part_extent_census(double alpha) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_PART_EXTENT_CENSUS"); s = (e && *e && *e != '0'); }
    if (!s) return;
    char buf[512]; int n = 0;
    for (int cl = 0; cl < 16; cl++) {
        int cnt = 0, dsz = 0; double ext = 0, vz = 0;
        for (int i = 0; i < s_item_n; i++) {
            const part_item* it = &s_item[i];
            if (it->cls != cl || !it->rec) continue;
            cnt++; ext += part_item_extent(it); vz += it->vz;
            if (s_reb[i].dsz) dsz++;
        }
        if (cnt) n += snprintf(buf + n, sizeof buf - (size_t)n,
                               " c%d:n=%d ext=%.2f vz=%.1f dsz=%d",
                               cl, cnt, ext / cnt, vz / cnt, dsz);
    }
    fprintf(stderr, "[part-ext f=%u a=%.3f prims=%d]%s\n",
            openpete_frame, alpha, s_item_n, buf);
}


int particles_replay_prev(int on) {
    if (!on) {
        if (s_item_held == CUT_HELD_PREV) part_item_swap();
        s_item_ref_prev = 0;
        s_item_held = -1;
        return 0;
    }
    if (s_item_held >= 0) return s_item_held != CUT_HELD_EMPTY;
    const cut_held_pick_t pick = cut_held_set_pick(s_item_epoch, s_item_prev.epoch);
    s_item_held = (int)pick;
    if (pick == CUT_HELD_PREV) {
        s_item_ref_cut = s_item_ref_ns;
        part_item_swap();
        s_item_ref_prev = 1;
    } else if (pick == CUT_HELD_CURR) {
        /* No capture in tick N: the moby cull's stamp is that tick's draw stamp. */
        s_item_ref_prev = moby_native_canon_ref_ns(&s_item_ref_cut);
    }
    cut_census_held_set("particles", pick, s_item_epoch, pick == CUT_HELD_EMPTY ? 0 : s_item_n);
    return pick != CUT_HELD_EMPTY;
}

/* OPENPETE_LATE_CENSUS: over the presents that drew the previous epoch's set
 * with the newest set captured (a late present or a held cut), the presents
 * whose set differs from the newest by identity, the records held at their
 * previous state (slot freed, reused or unpaired at the newest tick), and the
 * newest set's records the present left out (a slot and serial the previous
 * set does not hold), of which `reused` took a slot the previous set holds. */
static unsigned long s_late_presents, s_late_changed, s_late_held, s_late_deferred, s_late_reused;

/** @brief Print the late-switch counts at exit. */
static void part_late_report(void) {
    OP_ROLE_ASSERT(sim);
    fprintf(stderr, "[late-switch] kind=part presents=%lu changed=%lu held=%lu deferred=%lu reused=%lu\n",
            s_late_presents, s_late_changed, s_late_held, s_late_deferred, s_late_reused);
}

/** @brief Adds one present to the OPENPETE_LATE_CENSUS counts when it draws the previous epoch's set; reads the variable once.
 *
 * Runs after the rebase, whose verdicts name the held records. */
static void part_late_census(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("OPENPETE_LATE_CENSUS");
        on = (e && *e && *e != '0');
        if (on) atexit(part_late_report);
    }
    if (!on || s_item_held != CUT_HELD_PREV) return;
    unsigned long held = 0, deferred = 0, reused = 0;
    for (int i = 0; i < s_item_n; i++)
        if (s_item[i].rec && s_reb[i].active && s_reb[i].verdict != PART_PAIR_LERP) held++;
    for (int j = 0; j < s_item_prev.n; j++) {
        const part_item* nw = &s_item_prev.item[j];
        if (!nw->rec) continue;
        int same = 0, slot = 0;
        for (int i = 0; i < s_item_n && !same; i++) {
            if (s_item[i].rec != nw->rec) continue;
            slot = 1;
            same = s_item[i].serial == nw->serial;
        }
        if (!same) { deferred++; reused += (unsigned long)slot; }
    }
    s_late_presents++;
    s_late_changed += (held || deferred) ? 1u : 0u;
    s_late_held += held;
    s_late_deferred += deferred;
    s_late_reused += reused;
}

/* Sub-tick re-extract of the captured particle set; see particles.h. */
void particles_reextract(CPUState* cpu, int bracketed, double alpha) OP_REQUIRES(sim) {
    if (!hook_reextract_ready_held(RP_PARTICLES, s_item_held)) return;
    if (bracketed) part_rebase_all(cpu, alpha);
    part_late_census();
    s_feed_alpha = bracketed ? alpha : -1.0;
    part_extent_census(alpha);
    render_material_level_push(cpu);
    part_feed_native();
}

/* ---- slot allocations ------------------------------------------------------ */

int particles_slot(uint32_t rec) {
    const uint32_t stride = game_layout.particles.rec_stride;
    if (!s_slot_base || !stride || rec < s_slot_base) return -1;
    const uint32_t off = rec - s_slot_base;
    if (off % stride) return -1;
    const uint32_t idx = off / stride;
    return idx < PART_SLOTS_MAX ? (int)idx : -1;
}

uint32_t particles_slot_serial(uint32_t rec) {
    const int i = particles_slot(rec);
    return i < 0 ? 0u : s_slot_serial[i];
}

/** @brief The particle allocator hook: runs the guest allocator once, then stamps the slot it returned with a new allocation serial.
 *
 * The allocator (S2 asm/updatemobys.s func_80041C50, S3 func_80056434)
 * returns the slot in v0 on both of its paths: the slot at the pool cursor,
 * or, with the pool full, a random slot whose class byte has bit 4 clear.
 * Every spawn takes its slot through it. Declines only on the re-entry the
 * module dispatch below makes. A change of pool base clears every serial and
 * every owner tag. The slot is also handed to player_hide_note_alloc, which
 * tags its owner.
 * @param cpu The guest CPU state.
 * @param addr The allocator's entry address.
 * @return 1 when the allocator ran here, else 0. */
static int on_part_alloc(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    if (s_alloc_in) return 0;
    s_alloc_in = 1;
    { OP_PROF_FLAGS_ACCUM_SCOPE("particles.alloc", OP_PROF_F_GUEST); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_GUEST);
      openpete_recomp_module_get()->dispatch(cpu, addr); }
    s_alloc_in = 0;
    const uint32_t base = cpu->read_word(game_layout.particles.array_ptr);
    int moved = 0;
    if (base != s_slot_base) {
        memset(s_slot_serial, 0, sizeof s_slot_serial);
        s_slot_base = (base >= 0x80000000u && base < 0x80800000u) ? base : 0u;
        moved = 1;
    }
    const int i = particles_slot(cpu->v0);
    if (i >= 0) s_slot_serial[i] = alloc_serial_next(&s_alloc_serial);
    player_hide_note_alloc(i, moved);
    return 1;
}

/** @brief The particle free hook: clears the allocation serial of the slot in a0, then declines so the guest routine frees it.
 *
 * The free routine (S2 asm/updatemobys.s func_80041D20, S3 func_80056504)
 * marks the record at a0 free: class byte -2, or the pool's end sentinel -1
 * when every slot after it is free. A record later written into the slot
 * outside the allocator then pairs with serial 0 against the old record's
 * nonzero serial, which part_record_pair reads as a reuse.
 * @param cpu The guest CPU state; a0 is the record.
 * @param addr The free routine's entry address.
 * @return 0: the guest routine runs. */
static int on_part_free(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    (void)addr;
    const int i = particles_slot(cpu->a0);
    if (i >= 0) { s_slot_serial[i] = 0u; s_free_hits++; }
    return 0;
}

/* ---- hook ----------------------------------------------------------------- */
static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_PARTICLES, .prof_tag = "particles.srend",
    .st = &s_hb,
};

/** @brief Prints the widened-view census totals to stderr; registered at exit by part_wide_census_on. */
static void part_wide_total_report(void) {
    OP_ROLE_ASSERT(sim);
    const part_wide_tally* t = &s_wide_total;
    fprintf(stderr, "[part-wide total] cand=%lu admit=%lu deep=%lu cull=%lu mism=%lu recs=%lu exh=%lu "
                    "syn=%lu syn_bad=%lu syn_c=%lu/%lu/%lu/%lu/%lu\n",
            t->cand, t->admit, t->deep, t->cull, t->mism, t->recs, t->exh, t->syn, t->syn_bad,
            t->syn_c[0], t->syn_c[1], t->syn_c[2], t->syn_c[3], t->syn_c[4]);
}

/** @brief Whether the widened-view census covers this VBL; registers the total line at exit on the first covered one.
 * @return 1 inside the range OPENPETE_PART_WIDE_CENSUS names. */
static int part_wide_census_on(void) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    static int registered;
    if (!env_frame_range_active(&s_range, "OPENPETE_PART_WIDE_CENSUS", openpete_frame)) return 0;
    if (!registered) { registered = 1; atexit(part_wide_total_report); }
    return 1;
}

/** @brief Adds the walk's widened-view tally to the run totals and prints its line.
 *
 * @param fed The native feed's emission: the items the set holds after the walk. */
static void part_wide_census_print(int fed) {
    s_wide_total.cand += s_wide.cand;
    s_wide_total.admit += s_wide.admit;
    s_wide_total.deep += s_wide.deep;
    s_wide_total.cull += s_wide.cull;
    s_wide_total.mism += s_wide.mism;
    s_wide_total.recs += s_wide.recs;
    s_wide_total.exh += s_wide.exh;
    s_wide_total.syn += s_wide.syn;
    s_wide_total.syn_bad += s_wide.syn_bad;
    for (int c = 0; c < 8; c++) s_wide_total.syn_c[c] += s_wide.syn_c[c];
    fprintf(stderr, "[part-wide f=%u] cand=%lu admit=%lu deep=%lu cull=%lu mism=%lu recs=%lu exh=%lu items=%d\n",
            openpete_frame, s_wide.cand, s_wide.admit, s_wide.deep, s_wide.cull, s_wide.mism, s_wide.recs,
            s_wide.exh, fed);
}

/** @brief Whether the routine may have stopped before the pool's end because the polybuf filled.
 *
 * The routine ends its walk once the cursor comes within 0x400 bytes of the
 * polybuf's end (Spyro 2 partsdraw.s:54-56, :67, :93-94), so the records it
 * never reached keep the previous call's stamp.
 * @param cpu Reads the polybuf's end word.
 * @param endp The cursor the routine left.
 * @return 1 when the cursor reached that limit. */
static int part_polybuf_exhausted(CPUState* cpu, uint32_t endp) {
    if (!game_layout.polybuf.end) return 0;
    const uint32_t limit = cpu->read_word(game_layout.polybuf.end) - 0x400u;
    return (int32_t)(limit - endp) <= 0;
}

/** @brief Captures the routine's primitives, pairs them, appends the widened view's records, and feeds the set.
 *
 * Captures the polybuf interval (an empty one resets the set), builds the
 * tick's view and walks the pool (part_pair_depths); in a view wider than
 * 4:3 (ngte_view.extended) or under extended LOD (openpete_no_lod) the walk
 * appends the records part_reveal_record reveals, unless the routine stopped
 * at the polybuf's end. The census lines print, and the
 * native scene is fed when it is live and RP_PARTICLES is enabled. A set the
 * routine appended nothing for marks RP_PARTICLES drawn (render_path_mark_drawn),
 * since only a polybuf interval or the mark makes the sub-tick re-extract feed
 * the set.
 * @param cpu Reads guest memory, writes none.
 * @param startp The interval's start.
 * @param endp The interval's end.
 * @param feed Nonzero when the bracket feeds the native scene. */
static void part_draw_capture(CPUState* cpu, uint32_t startp, uint32_t endp, int feed) OP_REQUIRES(sim) {
    part_capture_prims(cpu, startp, endp);
    const uint32_t fed_before = s_fed_tris;
    ngte_view view;
    ngte_view_build(cpu, &view);
    part_dbg_latch();
    if (s_reb_dbg >= 2) part_view_dbg("tick", &view);
    const int exhausted = part_polybuf_exhausted(cpu, endp);
    const int census = part_wide_census_on();
    memset(&s_wide, 0, sizeof s_wide);
    s_wide.exh = (unsigned long)exhausted;
    part_pair_depths(cpu, &view, (view.extended || openpete_no_lod) && !exhausted,
                     census && !exhausted);
    if (census) part_wide_census_print(s_item_n);
    if (part_vdbg_on())
        for (int i = 0; i < s_item_n; i++) part_vertex_calib(&s_item[i]);
    part_extent_census(-1.0);
    render_material_level_push(cpu);
    s_feed_alpha = -1.0;
    if (feed && render_path_enabled(RP_PARTICLES)) {
        part_feed_native();
        part_vertex_jitter();
    }
    if (endp <= startp && s_item_n > 0) render_path_mark_drawn(RP_PARTICLES);
    part_census(startp, endp, fed_before);
}

/** @brief The particle draw hook: runs the guest draw once, then captures and feeds its primitives.
 *
 * Through hook_bracket_run the guest routine runs unchanged and reports the
 * polybuf interval it appended. When the interval is not empty, or the view
 * is wider than 4:3 or extended LOD is on and so can show a record the
 * routine appended nothing for,
 * part_draw_capture captures, pairs and feeds the set. Guest memory is read
 * after the draw, never written.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the bracket declines and the ordinary dispatch must run the
 *         draw, else 1: the draw has run. */
static int on_particles_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;
    if (endp > startp || ngte_view_extended(cpu) || openpete_no_lod)
        part_draw_capture(cpu, startp, endp, feed);
    return hook_bracket_end(&k_bracket);
}

/* The hook address is the layout's particle draw site, bound at boot: a static
 * initializer cannot read another translation unit's const. */
static feature_dispatch_entry_t particles_dispatch[] = {
    { 0u, NULL, 0u },
    { 0u, NULL, 0u },
    { 0u, NULL, 0u },
    { 0u, NULL, 0u },
};

/** @brief Binds the dispatch table's hook addresses to the layout's particle draw site, slot allocator and slot free routine.
 *
 * The table ends at the first zero address, so only the sites the layout
 * names are entered, in order.
 * @param opts Unused. */
static void particles_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    int n = 0;
    if (game_layout.particles.hook)
        particles_dispatch[n++] = (feature_dispatch_entry_t){ game_layout.particles.hook, on_particles_draw, 0u };
    if (game_layout.owner.part_alloc && game_layout.particles.array_ptr)
        particles_dispatch[n++] = (feature_dispatch_entry_t){ game_layout.owner.part_alloc, on_part_alloc, 0u };
    if (game_layout.owner.part_free && game_layout.owner.part_alloc && game_layout.particles.array_ptr)
        particles_dispatch[n++] = (feature_dispatch_entry_t){ game_layout.owner.part_free, on_part_free, 0u };
}

const feature_module_t particles_module = {
    .name           = "particles_native",
    .apply_options  = particles_apply_options,
    .dispatch_table = particles_dispatch,
};
