// games/common/renderer/glow.c - the glow-ring feeder: a handwritten routine
// (Spyro 2 func_8004A8D4 / mobyfunc.s:1936-2146, Spyro 3 func_8002ECA8 /
// mobyfunc.s:1920-2166; the same 208 instructions but for constants and
// symbols) that draws one screen-facing polygon per live record of a pool and
// links each fan into the 3D ordering table itself, so none of its primitives
// reaches the 2D chokepoint the sprite feeder hooks. Every per-game address,
// count, stride and bound comes from game_layout.glow; the walk below is one
// body.
//
// Read from the routine (asm/mobyfunc.s):
//   * the pool is 16 records of 0x14 bytes: world XYZ at +0x00, the primitive
//     colour at +0x0C, and a packed word at +0x10 holding the shape index, the
//     size, a rotation phase and a signed ordering-table bias (:1928-1929,
//     :1946-1951, :2002-2003).
//   * the shape comes from a table of 64-byte entries indexed by the shape byte
//     (:2008-2012): sixteen packed direction pairs, then sixteen halfwords of
//     vertex-index pairs ended by a zero, so a record draws up to sixteen
//     triangles fanned from its centre.
//   * the rotation is a 2x2 basis built from the sine table at the camera's own
//     yaw plus the record's phase, loaded into the GTE's rotation registers
//     (:2043-2058), so the polygon always faces the screen.
//   * each record emits one bundled draw-mode mini-packet (0xE1000620, whose
//     ABR bits are 0b01, additive) followed by its triangles, all chained into
//     one ordering-table entry (:2098-2110, :2170-2178).
//   * the triangles are POLY_G3 semi-transparent with the record's colour at
//     the centre vertex and black at the two rim vertices, so the ring is
//     brightest at its middle and fades out (:2110-2126).
//
// The fans are not read back out of the polybuf. The routine's own rejects are
// mirrored on the pool (glow_classify) to say which records it drew, and every
// drawn record's fan is built in float from the record, the shape table, the
// sine table and the camera (glow_fan_build), at capture under the tick's view
// and again at each sub-tick present under the staged view, from the record
// position, phase and camera yaw that present shows. The routine's integer
// corners carry the floors of its >> 6 basis scale, its RTPS and its yaw >> 4,
// which change from tick to tick; the float fan has none of them. What the
// primitives would not carry either is depth - the PSX orders them by
// ordering-table bucket - so each fan carries the key its record links at
// (glow_record_depth): its stored depth is the front edge of that bucket, which
// orders it against terrain as the table does, and the key itself contests the
// actor key image per pixel (the aura contest, psx_key_contest.glsl
// op_aura_loses), which orders it against every moby face by that face's own
// bucket, depth offset included. A sub-tick present takes the depth from the
// bucket of the view Z its record shows at under the present's camera, so
// the fan meets terrain where that present draws it; the key it contests by
// stays the tick's.
//
// The routine's lateral cull is the 4:3 frame plus a fixed margin. In a wider
// view a record it dropped on that test alone is drawn too unless its fan has
// left the view's screen-X window (glow_wide_admit).
//
// Under extended LOD, which draws mobys past the game's own draw distances,
// a record the routine dropped on its box or far bound alone is drawn too
// (glow_view_verdict). Some classes hold a record only while a child moby,
// or the moby itself, was drawn on the previous tick (game_layout.glow.synth);
// where the game freed it and native still draws that moby, the record is
// built natively as the game writes it (glow_synth_moby), at a position the
// game reads off the child or off the moby's own animated model vertices
// (glow_part_pos). Original LOD runs neither.
//
// Spyro 1's own glow renderer is fed by a float re-derivation in
// games/spyro-1/renderer/particle.c: its records carry a moby-position pointer
// and its ring is a perspective-scaled radius, a different routine over a
// different record shape. What the two share - the hook bracket and the
// projection helpers - is already shared and is reused here unchanged.
//
// Out of scope: the same routine reached from the pause screen's own draw,
// which stays an existing carve-out; this hook is the one call the world draw
// makes.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the phase byte it
// decrements in each record; this feeder only reads them afterwards.

#include "glow.h"
#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "renderer/cut_guard.h"     /* cut_held_sets_t: the held cut's tick N-1 set */
#include "renderer/player_hide.h"   /* --hide-player: the companion's ring */
#include "renderer/interp_regions_gen.h"   /* the camera row of the interp ring */
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_interp.h"        /* the camera snapshot pair */
#include "openpete_interp_objs.h"   /* the per-record prev/curr pool snapshots */
#include "game_layout.h"
#include "renderer/subtick_anchor_chk.h"
#include "renderer/moby_game.h"         /* moby_rec_holds_moby: the animated mobys native draws */
#include "renderer/moby.h"              /* moby_native_simple_mobys: the simple mobys native draws */
#include "no_lod.h"                     /* openpete_no_lod: the extended-LOD clauses */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

extern unsigned openpete_frame;

#define GW              (game_layout.glow)
#define GLOW_SHAPE_MASK 0xFFu

#define GLOW_MAX_PRIMS  512
#define GLOW_DRMODE     0xE1u   /* GP0 draw-mode packet, one per accepted record */
#define GLOW_TRI        0x32u   /* POLY_G3 semi-transparent */
#define GLOW_RIM_N      16
#define GLOW_TRI_N      16
#define GLOW_REC_MAX    32      /* records one call can draw: at least GW.slots */

/* One drawn record as its fan is rebuilt from: the depth its triangles take
 * under the tick's view, the slot address the keyed store holds its previous
 * copy under, the position words and packed word the tick read (the phase
 * byte as the routine left it), and the camera yaw halfword the routine read. */
typedef struct {
    float    depth;
    int      key;        /* the bucket it links at, -1 for the front entry */
    uint32_t addr;
    double   w[3];       /* the world position; integral but for a model-vertex glow */
    uint32_t packed;
    uint32_t rgb;        /* the centre colour, low 24 bits */
    int      yaw;
    int      synth;      /* a native glow keyed by its moby (glow_synth_moby) */
    const game_glow_synth_t* part;   /* its class's entry for a model-vertex glow, else NULL */
} glow_rec;

/** @brief The signed ordering-table bias byte of a record's packed word.
 * @param packed The packed word.
 * @return The bias. */
static int glow_bias(uint32_t packed) {
    return (int)(int8_t)(uint8_t)((packed >> 24) & 0xFFu);
}

/* One fed triangle. `p` and `depth` carry the positions and depth this
 * present draws; cx/cy and src.depth keep the ones built at capture, which a
 * held cut and a present past the near plane show. gx/gy are the routine's own corners, read only by the vertex
 * probe (glow_vtx_*) and never fed. */
typedef struct {
    ngte_prim p;
    float     cx[3], cy[3];
    float     depth;
    int       blend;
    int       tri;        /* the triangle's index in its record's fan */
    glow_rec  src;
    int       sparx;      /* its record is the companion's (player_hide_sparx_glow) */
    int       has_g;
    float     gx[3], gy[3];
    double    csx, csy;   /* the record centre's screen point at capture */
} glow_item;

static glow_item  s_item_buf[2][GLOW_MAX_PRIMS];
static glow_item* s_item = s_item_buf[0];
static int        s_item_n;
/* The set the last capture of a new canonical epoch retired, which a held
 * camera cut shows when it is tick N-1's. */
static glow_item* s_item_old = s_item_buf[1];
static int        s_item_old_n;
static int       s_groups;        /* draw-mode packets the call appended (instrument walk) */
static int       s_accepted;      /* records the pool walk says the routine drew */
static uint32_t  s_code_hist[256];
static uint32_t  s_tag_hist[256];
static uint32_t  s_fed_tris;
static unsigned  s_epoch;         /* captures taken, for the vertex probe */

/** @brief Exchange the current captured set with the retired one, with their counts. */
static void glow_sets_swap(void) {
    glow_item* t = s_item; s_item = s_item_old; s_item_old = t;
    const int n = s_item_n; s_item_n = s_item_old_n; s_item_old_n = n;
}

static cut_held_sets_t s_sets = { glow_sets_swap, &s_item_n, "glow", { 0u, 0u }, 0u };

/* ---- the routine's own primitives, for instruments only ------------------ */

static ngte_prim s_guest[GLOW_MAX_PRIMS];
static int       s_guest_n;

/** @brief Append one walked primitive to s_guest.
 * @param ctx Unused.
 * @param s The decoded primitive.
 * @return 1 when stored, 0 when s_guest holds GLOW_MAX_PRIMS. */
static int glow_guest_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_guest_n >= GLOW_MAX_PRIMS) return 0;
    s_guest[s_guest_n++] = *s;
    return 1;
}

/** @brief Walk the polybuf interval the glow routine allocated into s_guest and count its fans into s_groups.
 *
 * The interval holds 3-word draw-mode packets (tag length 2) and 7-word
 * triangles (tag length 6); a longer tag ends the walk. Only the census, the
 * cull log and the vertex probe read the result; the feed never does.
 * @param cpu The guest CPU state, read only.
 * @param start First polybuf address of the routine's output.
 * @param end One past the last. */
static void glow_capture_guest(CPUState* cpu, uint32_t start, uint32_t end) {
    s_guest_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 6, .raw_words_max = 7, .guard = GLOW_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, glow_guest_sink, NULL);
    for (int i = 0; i < s_guest_n; i++)
        if (s_guest[i].code == GLOW_DRMODE) s_groups++;
}

/** @brief Whether the fans contest the actor key image by their bucket: while the key pass runs.
 *
 * @return 1 when the fans carry their key, else 0. */
static int glow_key_on(void) {
    return openpete_native_key_resolve_on();
}

/** @brief The fan's native depth and its ordering-table key, from the key the routine links it at.
 *
 * The key is (MAC3 >> 7) - bias (Spyro 2 mobyfunc.s:2048-2049, Spyro 3 :2052-2053). A
 * negative key links at bucket 0 (Spyro 2 :2050-2052); on a game with the
 * front-entry branch, a record whose bias byte is front_bias links one entry
 * before bucket 0 instead (Spyro 3 :2054-2059), which is drawn after the whole
 * table and so takes the nearest depth and key -1. With the key pass on, the
 * depth is the front edge of the key's own bucket on the moby feeders' scale
 * (openpete_worldvz_owner_slot): against terrain, whose native depth carries
 * no authored pull, that is the table's verdict, bias slots nearer than the
 * record; against a moby face the key decides per pixel instead (the aura
 * contest, glow_feed_tri), since a face nearer than the bucket's front edge
 * links at a nearer bucket unless its depth offset is negative. Without the
 * key pass the fan sorts by that depth alone. The fan is additive on a
 * pipeline that writes no depth (native_render.c gouraud_add_desc), so this
 * depth decides only which opaque pixels it adds onto; it occludes nothing.
 * @param m3 The record's view-space MAC3.
 * @param bias The record's signed ordering-table bias byte.
 * @param key Receives the bucket the record links at, -1 for the front entry.
 * @return The scene depth the fan's triangles take. */
static float glow_record_depth(double m3, int bias, int* key) {
    int k = (int)((uint32_t)(int32_t)m3 >> 7) - bias;
    if (k < 0) {
        if (GW.front_bias >= 0 && bias == GW.front_bias) {
            *key = -1;
            return openpete_native_scene_viewz(openpete_worldvz_full(1.0f));
        }
        k = 0;
    }
    *key = k;
    return openpete_native_scene_viewz(openpete_worldvz_owner_slot(k));
}

/* The routine's verdict on one pool record, in the order it tests them. */
typedef enum {
    GLOW_V_FREE, GLOW_V_BOX, GLOW_V_NEAR, GLOW_V_FAR, GLOW_V_PYR_X, GLOW_V_PYR_Y,
    GLOW_V_ACCEPT,
} glow_verdict;

static const char* const k_glow_verdict_name[] = {
    "free", "box", "near", "far", "pyr_x", "pyr_y", "accept",
};

/* One pool record as the routine's cull reads it: the packed word, the
 * position words, the camera-relative offset (X - camX, camY - Y, camZ - Z)
 * and its rotation by the camera matrix. */
typedef struct {
    uint32_t addr;
    uint32_t packed;
    uint32_t rgb;
    int      synth;
    const game_glow_synth_t* part;
    double   w[3];
    double   off[3];
    double   m1, m2, m3;
} glow_src;

/** @brief Apply the routine's rejects to one record's position in the routine's order.
 *
 * The camera-relative offset must stay inside the +/-GW.box box
 * (:1970-1988); the rotated depth must be at least GW.near_z and under
 * GW.far_z; the rotated point must stay inside the view pyramid
 * 4|X| - 3Z < GW.pyr_x and 3|Y| - Z < GW.pyr_y (:1994-2000). The pyramid is
 * the 4:3 frame widened by a margin of GW.pyr_x / 4 view units laterally.
 * With `ranged` the box and the far bound are skipped, which is the
 * extended-LOD reading of the same tests.
 * @param v The tick's projection view.
 * @param r The record with its position words; receives the offset and, past
 *        the box test, the rotation.
 * @param ranged Nonzero to skip the box and far tests.
 * @return The verdict. */
static glow_verdict glow_cull(const ngte_view* v, glow_src* r, int ranged) {
    r->off[0] = (double)r->w[0] - v->cam[0];   /* the routine's IR3 */
    r->off[1] = v->cam[1] - (double)r->w[1];   /* IR1 */
    r->off[2] = v->cam[2] - (double)r->w[2];   /* IR2 */
    const double box = (double)GW.box;
    for (int k = 0; k < 3 && !ranged; k++)
        if (r->off[k] <= -box || r->off[k] >= box) return GLOW_V_BOX;
    ngte_mac(v->cam_M, r->off[1], r->off[2], r->off[0], &r->m1, &r->m2, &r->m3);
    if (r->m3 < (double)GW.near_z) return GLOW_V_NEAR;
    if (!ranged && r->m3 >= (double)GW.far_z) return GLOW_V_FAR;
    if (4.0 * fabs(r->m1) - 3.0 * r->m3 - (double)GW.pyr_x >= 0.0) return GLOW_V_PYR_X;
    if (3.0 * fabs(r->m2) - r->m3 - (double)GW.pyr_y >= 0.0) return GLOW_V_PYR_Y;
    return GLOW_V_ACCEPT;
}

/** @brief Read one pool record and apply the routine's rejects to it (glow_cull).
 *
 * A zero shape byte is a free slot (:1932).
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view.
 * @param s The pool slot.
 * @param r Receives the record; the offset and rotation only past the free test.
 * @return The routine's verdict. */
static glow_verdict glow_classify(CPUState* cpu, const ngte_view* v, uint32_t s, glow_src* r) {
    r->addr = GW.pool + s * GW.stride;
    r->packed = cpu->read_word(r->addr + GW.packed_off);
    r->synth = 0;
    r->part = NULL;
    if ((r->packed & GLOW_SHAPE_MASK) == 0u) return GLOW_V_FREE;
    r->rgb = cpu->read_word(r->addr + 0x0Cu) & 0xFFFFFFu;
    for (uint32_t k = 0; k < 3u; k++)
        r->w[k] = (double)(int32_t)cpu->read_word(r->addr + GW.pos_off + k * 4u);
    return glow_cull(v, r, 0);
}

/** @brief Whether the routine skips a live record before its cull in its front-entry-only state.
 *
 * True when the layout names the branch, the game state word equals
 * GW.solo_state, the flag word is non-zero and the record's bias byte is not
 * GW.front_bias.
 * @param cpu The guest CPU state, read only.
 * @param packed The record's packed word.
 * @return 1 when the routine draws nothing for the record, else 0. */
static int glow_solo_skips(CPUState* cpu, uint32_t packed) {
    if (!GW.solo_flag) return 0;
    if (cpu->read_word(GW.solo_state_addr) != GW.solo_state) return 0;
    if (cpu->read_word(GW.solo_flag) == 0u) return 0;
    return (int)(int8_t)(uint8_t)((packed >> 24) & 0xFFu) != GW.front_bias;
}

/* ---- the fan, built from the record --------------------------------------
 *
 * The fan the routine draws for one record, from the record and the camera
 * alone:
 *   * the shape entry is shape_table + (shape byte - 1) * 64: sixteen
 *     halfwords each packing a signed rim direction (X in the low byte, Y in
 *     the high byte), then up to sixteen vertex-index halfwords (rim A in bits
 *     8-11, rim B in bits 0-3) ended by a zero (Spyro 3 mobyfunc.s:2013-2018,
 *     :2122-2143).
 *   * the angle is ((camera yaw >> 4) + phase) & 0xFF, where the yaw is the
 *     camera's s16 at cam_yaw_off and the phase is packed byte 2 as the
 *     routine leaves it (it decrements the byte before any cull, :1981-1982);
 *     the sine table gives s at the angle and c a quarter turn on (+0x80
 *     bytes), and with c' = size * c / 64 and s' = size * s / 64 the rim
 *     direction (x, y) moves the centre by ((-1.5 c' x + 1.5 s' y) / 4096,
 *     (s' x + c' y) / 4096) in view X and Y at the centre's own view Z
 *     (:2062-2106).
 *   * each triangle is (centre, rim A, rim B), the centre in the record's
 *     colour word and both rims black, additive (:2110-2143).
 * The angle here is yaw / 16 + phase, unfloored, read through ngte_sincos_f,
 * so the ring turns with the camera between yaw steps of 16. */

/* One built fan in screen space, under the view it was built for, with the
 * depth its triangles take there (glow_record_depth of the centre's view Z,
 * set by glow_fan_at). */
typedef struct {
    int      n;                         /* triangles */
    float    depth;
    float    x[GLOW_TRI_N][3], y[GLOW_TRI_N][3];
    uint32_t rgb;                       /* the centre colour, low 24 bits */
    float    xmin, xmax;
    double   csx, csy;                  /* the centre's screen point */
} glow_fan;

static int s_wide_admit;   /* fans the widened view built and fed this call */
static int s_wide_cull;    /* fans the widened view built and culled this call */
static int s_range_admit;  /* box- or far-culled records extended LOD fed this call */
static int s_synth_admit;  /* native glows extended LOD fed for a freed record this call */

/** @brief The camera yaw halfword the routine reads, sign-extended.
 * @param cpu The guest CPU state, read only.
 * @return The yaw, 4096 to the turn in its low 12 bits. */
static int glow_cam_yaw(CPUState* cpu) {
    return (int)(int16_t)cpu->read_half(game_layout.cam.base + GW.cam_yaw_off);
}

/** @brief The sine-table angle a fan turns to, from a camera yaw and a phase, unfloored.
 * @param yaw The camera yaw, fractional on a sub-tick present.
 * @param phase The record's phase byte, fractional on a sub-tick present.
 * @return The angle in table entries, 256 to the turn, not wrapped. */
static double glow_angle(double yaw, double phase) {
    return yaw / 16.0 + phase;
}

/** @brief Build the fan the routine draws for one record at an angle, projected under the view.
 *
 * Reads the shape entry and the sine table as the routine does (see the
 * section comment) at the given angle, and projects every rim point through
 * ngte_screen_gte with no SXY bound, so the fan keeps its true X in a widened
 * view. The routine's RTPS also multiplies a stale VZ0 by a rotation element
 * that is 0 or -1; that term is dropped (at most 8 view units).
 * @param cpu The guest CPU state, read only.
 * @param v The projection view.
 * @param r The record with its view-space centre (m1, m2, m3) under `v`.
 * @param ang The angle in sine-table entries.
 * @param f Receives the fan; f->n = 0 when the layout names no shape table or
 *        sine table. */
static void glow_fan_build(CPUState* cpu, const ngte_view* v, const glow_src* r, double ang,
                           glow_fan* f) {
    f->n = 0;
    f->xmin = 1e30f; f->xmax = -1e30f;
    if (!GW.shape_table || !game_layout.sin_table) return;
    const uint32_t entry = GW.shape_table + ((r->packed & GLOW_SHAPE_MASK) - 1u) * 64u;
    const double size = (double)((r->packed >> 8) & 0xFFu);
    double sn, c;
    ngte_sincos_f(cpu, ang, &sn, &c);
    c *= size / 64.0;
    sn *= size / 64.0;
    double rx[GLOW_RIM_N], ry[GLOW_RIM_N];
    for (uint32_t j = 0; j < GLOW_RIM_N; j++) {
        const uint16_t h = cpu->read_half(entry + j * 2u);
        const double x = (double)(int8_t)(h & 0xFFu), y = (double)(int8_t)(h >> 8);
        ngte_proj o;
        ngte_screen_gte(r->m1 + (-1.5 * c * x + 1.5 * sn * y) / 4096.0,
                        r->m2 + (sn * x + c * y) / 4096.0, r->m3,
                        v->ofx, v->ofy, v, 0, &o);
        rx[j] = o.sx; ry[j] = o.sy;
    }
    ngte_proj ctr;
    ngte_screen_gte(r->m1, r->m2, r->m3, v->ofx, v->ofy, v, 0, &ctr);
    f->csx = ctr.sx; f->csy = ctr.sy;
    f->rgb = r->rgb;
    for (uint32_t k = 0; k < GLOW_TRI_N; k++) {
        const uint16_t h = cpu->read_half(entry + 0x20u + k * 2u);
        if (h == 0u) break;
        const uint32_t a = (h >> 8) & 0xFu, b = h & 0xFu;
        const double xs[3] = { ctr.sx, rx[a], rx[b] }, ys[3] = { ctr.sy, ry[a], ry[b] };
        for (int i = 0; i < 3; i++) {
            f->x[f->n][i] = (float)xs[i];
            f->y[f->n][i] = (float)ys[i];
            if (f->x[f->n][i] < f->xmin) f->xmin = f->x[f->n][i];
            if (f->x[f->n][i] > f->xmax) f->xmax = f->x[f->n][i];
        }
        f->n++;
    }
}

/** @brief Append a built fan to s_item as additive triangles owned by its record.
 *
 * Each triangle keeps its positions twice, as fed and as built at capture.
 * Stops at GLOW_MAX_PRIMS items.
 * @param r The record the fan was built from, under the tick's view.
 * @param yaw The camera yaw the fan was built at.
 * @param f The fan. */
static void glow_fan_append(const glow_src* r, int yaw, const glow_fan* f) {
    glow_rec rec;
    rec.depth = glow_record_depth(r->m3, glow_bias(r->packed), &rec.key);
    rec.addr = r->addr;
    for (int k = 0; k < 3; k++) rec.w[k] = r->w[k];
    rec.packed = r->packed;
    rec.rgb = r->rgb;
    rec.synth = r->synth;
    rec.part = r->part;
    rec.yaw = yaw;
    const int blend = ngte_abr_blend(1, NGTE_ABR_QUARTER_AS_ADD);   /* 0xE1000620: ABR 0b01 */
    for (int t = 0; t < f->n && s_item_n < GLOW_MAX_PRIMS; t++) {
        glow_item* it = &s_item[s_item_n++];
        memset(it, 0, sizeof *it);
        it->p.code = GLOW_TRI;
        it->p.nv = 3;
        it->p.gouraud = 1;
        it->p.stp = 1;
        for (int i = 0; i < 3; i++) {
            it->p.x[i] = it->cx[i] = f->x[t][i];
            it->p.y[i] = it->cy[i] = f->y[t][i];
        }
        it->p.rgb[0] = f->rgb;
        it->depth = rec.depth;
        it->blend = blend;
        it->tri = t;
        it->src = rec;
        it->csx = f->csx;
        it->csy = f->csy;
    }
}

/** @brief Whether the per-record cull log is on for this VBL (OPENPETE_GLOW_CULL_LOG=LO-HI).
 *
 * The range is in openpete_frame units, as every census range is.
 * @return 1 when on, else 0. */
static int glow_cull_log_on(void) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    return env_frame_range_active(&s_range, "OPENPETE_GLOW_CULL_LOG", openpete_frame);
}

/** @brief Whether the per-call census is on for this VBL (OPENPETE_GLOW_CENSUS=LO-HI).
 * @return 1 when on, else 0. */
static int glow_census_on(void) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    return env_frame_range_active(&s_range, "OPENPETE_GLOW_CENSUS", openpete_frame);
}

/* ---- owner census ----------------------------------------------------------
 *
 * The allocator (Spyro 2 func_8004A87C, Spyro 3 func_8002EC50) stores the
 * shape, size, colour, a random phase and the bias 4 and returns the record;
 * the caller keeps the pointer in its own moby's tag block (the word at moby
 * +0x00): at +0x08 on the Glimmer lamps (level overlay 0x80079BD8
 * `sw $v0, 0x8($s5)`, $s5 the tag loaded at 0x80079A5C) and at +0x10 on
 * Sparx; the Spyro 3 call sites store it at up to +0x5C of their base
 * register. The record holds no pointer back, so the census finds the
 * owner by that link: the moby whose tag block holds the record's address
 * at the smallest offset under GLOW_OWNER_SCAN, the block that starts
 * nearest below the word. A record no block links falls back to the nearest
 * live moby. The census also names the live moby nearest the record, the
 * one the fan sits on. Neither enters the feed: the key contest orders the
 * fan against every moby by that moby's own bucket. */

#define GLOW_OWNER_SCAN 0x60u   /* tag-block bytes searched for a record pointer */

/* One owner lookup: the linking moby (or the nearest one), the link's offset
 * in its tag block (-1 for a fallback) and the nearest live moby. */
typedef struct {
    uint32_t owner;
    int      link_off;
    uint32_t sits;
    double   sits_d2;
} glow_owner;

static unsigned long s_own_linked, s_own_nearest;

/** @brief Fold one moby into an owner lookup: its tag-block link to the record, and its distance from it.
 * @param cpu The guest CPU state, read only.
 * @param m The moby.
 * @param r The record.
 * @param o The lookup, updated. */
static void glow_owner_visit(CPUState* cpu, uint32_t m, const glow_src* r, glow_owner* o) {
    const uint32_t tag = cpu->read_word(m);
    if (tag >= 0x80000000u && tag < 0x80800000u)
        for (uint32_t off = 0; off < GLOW_OWNER_SCAN && (o->link_off < 0 || (int)off < o->link_off); off += 4u)
            if (cpu->read_word(tag + off) == r->addr) { o->owner = m; o->link_off = (int)off; break; }
    if (cpu->read_byte(m + game_layout.moby.state_off) & 0x80u) return;   /* dormant */
    double d2 = 0.0;
    for (uint32_t k = 0; k < 3u; k++) {
        const double d = (double)(int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u * k) - r->w[k];
        d2 += d * d;
    }
    if (o->sits == 0u || d2 < o->sits_d2) { o->sits = m; o->sits_d2 = d2; }
}

/** @brief Find a record's owner by its tag-block link, else the nearest live moby, and the moby it sits on.
 * @param cpu The guest CPU state, read only.
 * @param r The record.
 * @param o Receives the lookup; owner 0 when the moby array is unreadable. */
static void glow_owner_find(CPUState* cpu, const glow_src* r, glow_owner* o) {
    memset(o, 0, sizeof *o);
    o->link_off = -1;
    const uint32_t head = game_layout.moby.array_head;
    const uint32_t arr = game_layout.moby.array_direct ? head : cpu->read_word(head);
    if (arr < 0x80000000u || arr >= 0x80800000u) return;
    for (uint32_t i = 0; i < 1024u; i++) {
        const uint32_t m = arr + i * game_layout.moby.array_stride;
        if (cpu->read_byte(m + game_layout.moby.state_off) == game_layout.moby.state_term) break;
        glow_owner_visit(cpu, m, r, o);
    }
    if (o->link_off < 0) o->owner = o->sits;
    if (o->link_off < 0) s_own_nearest++; else s_own_linked++;
}

/** @brief A moby's authored depth offset (the signed byte at angle_off + 3, +0x47), 0 for no moby.
 * @param cpu The guest CPU state, read only.
 * @param m The moby, or 0.
 * @return The offset in ordering-table slots. */
static int glow_moby_dofs(CPUState* cpu, uint32_t m) {
    return m ? (int)(int8_t)cpu->read_byte(m + game_layout.moby.angle_off + 3u) : 0;
}

/** @brief Print one accepted record's owner census line.
 *
 * Names the owner (the tag-block link, or `nearest` for a fallback), its
 * class and depth offset, and the slot the record's key reaches against it
 * (key + offset); then the same for the live moby the record sits on; then
 * the slot the fed depth stands at and whether the fan contests by its key.
 * @param cpu The guest CPU state, read only.
 * @param slot The pool slot.
 * @param r The record.
 * @param rec The record as appended (glow_fan_append). */
static void glow_owner_line(CPUState* cpu, uint32_t slot, const glow_src* r, const glow_rec* rec) {
    glow_owner o;
    glow_owner_find(cpu, r, &o);
    const int od = glow_moby_dofs(cpu, o.owner), sd = glow_moby_dofs(cpu, o.sits);
    const uint32_t cls = game_layout.moby.class_off;
    char link[16];
    if (o.link_off >= 0) snprintf(link, sizeof link, "+0x%02X", (unsigned)o.link_off);
    else snprintf(link, sizeof link, "nearest");
    fprintf(stderr, "[glow-owner f=%u] slot=%u rec=%08X key=%d owner=%08X cls=%03X link=%s dofs=%d"
                    " owner_slot=%d sits=%08X sits_cls=%03X sits_dofs=%d sits_dist=%.0f sits_slot=%d"
                    " fed_slot=%.2f contest=%d linked=%lu nearest=%lu\n",
            openpete_frame, slot, r->addr, rec->key, o.owner, o.owner ? cpu->read_half(o.owner + cls) : 0u,
            link, od, rec->key + od, o.sits, o.sits ? cpu->read_half(o.sits + cls) : 0u, sd,
            sqrt(o.sits_d2), rec->key + sd, ((double)rec->depth + OPENPETE_MOBY_TIE_PULL_VZ) / OPENPETE_OT_SLOT_VZ,
            glow_key_on(), s_own_linked, s_own_nearest);
}

/** @brief OPENPETE_GLOW_VTX, latched on first use: 0 off, 1 the exit summary, 2 also per-tick lines.
 * @return The level. */
static int glow_vtx_level(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLOW_VTX"); v = (e && *e) ? atoi(e) : 0; }
    return v;
}

/** @brief The first triangle of the routine's own fan for one accepted record in s_guest.
 *
 * The routine's fans follow their draw-mode packets in the order it accepted
 * the records.
 * @param group The record's ordinal among the routine's accepted records.
 * @return The index in s_guest of the fan's first primitive after its
 *         draw-mode packet, or -1 when the group is absent. */
static int glow_guest_group(int group) {
    int g = -1;
    for (int i = 0; i < s_guest_n; i++) {
        if (s_guest[i].code != GLOW_DRMODE) continue;
        if (++g == group) return i + 1;
    }
    return -1;
}

/** @brief The largest vertex distance between a built fan and the routine's own fan for the same record.
 *
 * The routine's fan is the triangles after the `group`-th draw-mode packet of
 * s_guest, compared triangle by triangle in order.
 * @param f The built fan.
 * @param group The record's draw-mode packet ordinal in s_guest.
 * @param ntri Receives the routine's triangle count for the record.
 * @return The largest per-axis vertex distance in screen pixels, or -1 when
 *         the group is absent. */
static double glow_fan_cal(const glow_fan* f, int group, int* ntri) {
    *ntri = 0;
    int i = glow_guest_group(group);
    if (i < 0) return -1.0;
    double worst = 0.0;
    for (int t = 0; i < s_guest_n && s_guest[i].code != GLOW_DRMODE; i++, t++) {
        const ngte_prim* p = &s_guest[i];
        if (p->nv != 3) continue;
        (*ntri)++;
        if (t >= f->n) { worst = 1e9; continue; }
        for (int k = 0; k < 3; k++) {
            worst = fmax(worst, fabs((double)p->x[k] - (double)f->x[t][k]));
            worst = fmax(worst, fabs((double)p->y[k] - (double)f->y[t][k]));
        }
    }
    return worst;
}

/** @brief Print one record's line of the cull log.
 *
 * The line holds the routine's verdict, the verdict this view draws the
 * record under, its centre's screen point, its fan's screen-X extent and the
 * view's screen-X window. For a record the routine accepted, when the walked
 * fans count the accepted records, it also holds the built and the routine's
 * triangle counts and their largest vertex distance at the routine's integer
 * angle (glow_fan_cal).
 * @param v The tick's projection view.
 * @param slot The pool slot.
 * @param g The routine's verdict.
 * @param view The verdict this view draws the record under.
 * @param r The record.
 * @param f The fan built at the feed's angle; n = 0 when none was built.
 * @param fi The fan built at the routine's integer angle; n = 0 when none was built.
 * @param accepted The record's ordinal among the routine's accepted records. */
static void glow_cull_log_line(const ngte_view* v, uint32_t slot, glow_verdict g, const char* view,
                               const glow_src* r, const glow_fan* f, const glow_fan* fi,
                               int accepted) {
    ngte_proj o;
    ngte_project_offset(v, r->off, &o);
    int ntri = 0;
    const double cal = (g == GLOW_V_ACCEPT && fi->n > 0) ? glow_fan_cal(fi, accepted, &ntri) : -1.0;
    fprintf(stderr, "[glow-cull f=%u] slot=%u routine=%s view=%s F=%.4f sx=%.1f sy=%.1f m3=%.0f"
                    " fan_x=[%.1f,%.1f] window=[%.1f,%.1f] cal_tris=%d/%d cal_maxerr=%.2f\n",
            openpete_frame, slot, k_glow_verdict_name[g], view, (double)v->F, o.sx, o.sy, r->m3,
            (double)f->xmin, (double)f->xmax, (double)v->xlo, (double)v->xhi, f->n, ntri, cal);
}

/** @brief Draw a record the routine's lateral test dropped when its fan is still inside the view's screen-X window.
 *
 * The fan is culled when every vertex lies left of xlo or every vertex at or
 * right of xhi, else appended (glow_fan_append); s_wide_cull or s_wide_admit
 * counts the outcome. A fan with no triangles is neither.
 * @param v The tick's projection view.
 * @param r The record.
 * @param yaw The camera yaw the fan was built at.
 * @param f Its fan.
 * @return The verdict name the cull log prints. */
static const char* glow_wide_admit(const ngte_view* v, const glow_src* r, int yaw,
                                   const glow_fan* f) {
    if (f->n <= 0) return "reject";
    if (f->xmax < v->xlo || f->xmin >= v->xhi) { s_wide_cull++; return "wide_cull"; }
    glow_fan_append(r, yaw, f);
    s_wide_admit++;
    return "wide_admit";
}

/* ---- the vertex probe (OPENPETE_GLOW_VTX) ---------------------------------
 *
 * Three statistics, printed at exit:
 *   * cal: each corner of the routine's own fan minus the fan built in float
 *     at the routine's integer angle, per axis, at the capture tick; with the
 *     per-tick mean and the count of corners whose error changed from the
 *     previous tick. A construction that is the routine's reads a floor error
 *     that changes per tick; a zero or constant reading falsifies it.
 *   * place: per present, each fed corner's distance from the fan built at the
 *     state that present shows (the record position, phase and camera yaw
 *     blended toward the previous tick by 1 - alpha) under its view; and the
 *     same for the routine's corners displaced by their record centre's
 *     screen motion, the form the feed drew before it was built in float.
 *   * jump: each corner at the alpha-0 present of tick N + 1 minus the same
 *     corner at the alpha-1 present of tick N, which show the same instant;
 *     and the alpha-1 present minus the canonical present of the same tick
 *     (the identity). For the fed and the displaced-routine corners alike. */

typedef struct { unsigned long n; double sum, mn, mx; } glow_acc;

/** @brief Add one sample to a running count, sum, minimum and maximum.
 * @param a The accumulator.
 * @param d The sample. */
static void glow_acc_add(glow_acc* a, double d) {
    if (a->n == 0 || d < a->mn) a->mn = d;
    if (a->n == 0 || d > a->mx) a->mx = d;
    a->sum += d;
    a->n++;
}

enum { GV_FED, GV_OLD, GV_N };
static glow_acc      s_vtx_cal[2], s_vtx_tick[2];
static unsigned long s_vtx_cal_cmp, s_vtx_cal_chg;
static glow_acc      s_vtx_place[GV_N][2];   /* [form][canonical, sub-tick] */
static glow_acc      s_vtx_jump[GV_N];       /* alpha 0 of N+1 against alpha 1 of N */
static glow_acc      s_vtx_ident[GV_N];      /* alpha 1 against the canonical present */
static unsigned long s_vtx_yaw_cmp, s_vtx_yaw_moved;

/* Per (slot, triangle, corner): the last tick's calibration error, and the
 * fed and displaced-routine corners at the last canonical and alpha-1
 * presents, with the epoch each was taken at. */
typedef struct {
    unsigned cal_epoch;
    float    cal_dx, cal_dy;
    unsigned canon_epoch, a1_epoch;
    float    canon[GV_N][2], a1[GV_N][2];
} glow_vkey;
static glow_vkey s_vkey[GLOW_REC_MAX][GLOW_TRI_N][3];

/** @brief Print the probe's summary to stderr; registered with atexit. */
static void glow_vtx_report(void) {
    OP_ROLE_ASSERT(sim);
    static const char* const ax[2] = { "x", "y" };
    for (int k = 0; k < 2; k++)
        fprintf(stderr, "[glow-vtx cal %s] n=%lu mean=%.4f min=%.4f max=%.4f"
                        " tick_mean: n=%lu min=%.4f max=%.4f\n",
                ax[k], s_vtx_cal[k].n, s_vtx_cal[k].n ? s_vtx_cal[k].sum / (double)s_vtx_cal[k].n : 0.0,
                s_vtx_cal[k].mn, s_vtx_cal[k].mx, s_vtx_tick[k].n, s_vtx_tick[k].mn, s_vtx_tick[k].mx);
    fprintf(stderr, "[glow-vtx cal change] compared=%lu changed=%lu\n", s_vtx_cal_cmp, s_vtx_cal_chg);
    fprintf(stderr, "[glow-vtx yaw] presents=%lu moved=%lu\n", s_vtx_yaw_cmp, s_vtx_yaw_moved);
    static const char* const form[GV_N] = { "fed", "routine+shift" };
    static const char* const leg[2] = { "canon", "inter" };
    for (int f = 0; f < GV_N; f++) {
        for (int l = 0; l < 2; l++) {
            const glow_acc* a = &s_vtx_place[f][l];
            fprintf(stderr, "[glow-vtx place %s %s] n=%lu mean=%.6f max=%.6f\n", form[f], leg[l],
                    a->n, a->n ? a->sum / (double)a->n : 0.0, a->mx);
        }
        fprintf(stderr, "[glow-vtx jump %s] n=%lu mean=%.6f max=%.6f\n", form[f], s_vtx_jump[f].n,
                s_vtx_jump[f].n ? s_vtx_jump[f].sum / (double)s_vtx_jump[f].n : 0.0, s_vtx_jump[f].mx);
        fprintf(stderr, "[glow-vtx ident %s] n=%lu mean=%.6f max=%.6f\n", form[f], s_vtx_ident[f].n,
                s_vtx_ident[f].n ? s_vtx_ident[f].sum / (double)s_vtx_ident[f].n : 0.0, s_vtx_ident[f].mx);
    }
}

/** @brief Latch the probe and register its summary on first use.
 * @return The level, 0 when the probe is off. */
static int glow_vtx_on(void) {
    static int armed = 0;
    const int lv = glow_vtx_level();
    if (lv > 0 && !armed) { armed = 1; atexit(glow_vtx_report); }
    return lv;
}

/** @brief The probe's key slot of a record address, or -1 outside the pool.
 * @param addr The record address.
 * @return The slot. */
static int glow_vslot(uint32_t addr) {
    if (!GW.stride || addr < GW.pool) return -1;
    const uint32_t s = (addr - GW.pool) / GW.stride;
    return s < GLOW_REC_MAX ? (int)s : -1;
}

/** @brief Calibrate one accepted record's integer-angle fan against the routine's corners and keep those corners on its items.
 *
 * Adds each corner's routine-minus-float error to the cal statistic and to
 * the per-tick sums, counts the corners whose error changed since the
 * previous tick, and copies the routine's corners onto the record's items
 * (the last `f->n` items appended) for the place and jump statistics.
 * @param fi The record's fan at the routine's integer angle.
 * @param group The record's ordinal among the routine's accepted records.
 * @param slot The record's probe slot.
 * @param first The index in s_item of the record's first triangle.
 * @param sum Accumulates the per-tick sums of the X and Y errors.
 * @param n Accumulates the per-tick corner count. */
static void glow_vtx_cal_rec(const glow_fan* fi, int group, int slot, int first,
                             double sum[2], int* n) {
    int i = glow_guest_group(group);
    if (i < 0 || slot < 0) return;
    for (int t = 0; i < s_guest_n && s_guest[i].code != GLOW_DRMODE; i++) {
        const ngte_prim* p = &s_guest[i];
        if (p->nv != 3) continue;
        if (t >= fi->n || first + t >= s_item_n) break;
        glow_item* it = &s_item[first + t];
        it->has_g = 1;
        for (int k = 0; k < 3; k++) {
            const double dx = (double)p->x[k] - (double)fi->x[t][k];
            const double dy = (double)p->y[k] - (double)fi->y[t][k];
            it->gx[k] = p->x[k];
            it->gy[k] = p->y[k];
            glow_acc_add(&s_vtx_cal[0], dx);
            glow_acc_add(&s_vtx_cal[1], dy);
            sum[0] += dx; sum[1] += dy; (*n)++;
            glow_vkey* e = &s_vkey[slot][t][k];
            if (e->cal_epoch + 1u == s_epoch) {
                s_vtx_cal_cmp++;
                if (fabs(e->cal_dx - (float)dx) > 1e-4f || fabs(e->cal_dy - (float)dy) > 1e-4f)
                    s_vtx_cal_chg++;
            }
            e->cal_epoch = s_epoch;
            e->cal_dx = (float)dx;
            e->cal_dy = (float)dy;
        }
        t++;
    }
}

/* ---- capture ------------------------------------------------------------- */

/** @brief Build one record's fan at the routine's integer angle, for the cull log and the probe.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view.
 * @param r The record.
 * @param yaw The camera yaw the routine read.
 * @param fi Receives the fan. */
static void glow_fan_build_int(CPUState* cpu, const ngte_view* v, const glow_src* r, int yaw,
                               glow_fan* fi) {
    const uint32_t ang = (uint32_t)((yaw >> 4) + (int)((r->packed >> 16) & 0xFFu)) & 0xFFu;
    glow_fan_build(cpu, v, r, (double)ang, fi);
}

/* One capture's walk state: the view's switches, the camera yaw the routine
 * read, and per accepted record its integer-angle fan, its first triangle in
 * s_item and its pool slot, for the probe's calibration. */
typedef struct {
    int      wide, log, vtx, yaw;
    glow_fan fi[GLOW_REC_MAX];
    int      first[GLOW_REC_MAX], vslot[GLOW_REC_MAX];
} glow_capture_ctx;

/** @brief Whether extended LOD feeds a record the routine dropped on its box or far test, latched on first use.
 *
 * On by default; OPENPETE_GLOW_RANGE with a value starting with '0' turns it
 * off, for an A/B from one binary. Only read while extended LOD is on.
 * @return 1 when on, else 0. */
static int glow_range_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLOW_RANGE"); v = !(e && *e == '0'); }
    return v;
}

/** @brief The verdict the view draws a record under: the routine's, or under extended LOD the routine's tests without the box and far bounds.
 *
 * The routine's box and far bounds (GW.box, GW.far_z) stand inside the
 * distance extended LOD draws mobys at. A record dropped on either is tested
 * again by glow_cull without both, so the near and pyramid tests still apply.
 * @param v The tick's projection view.
 * @param r The record, re-rotated when tested again.
 * @param g The routine's verdict.
 * @return The verdict the view draws the record under. */
static glow_verdict glow_view_verdict(const ngte_view* v, glow_src* r, glow_verdict g) {
    if (g != GLOW_V_BOX && g != GLOW_V_FAR) return g;
    if (!openpete_no_lod || !glow_range_enabled()) return g;
    return glow_cull(v, r, 1);
}

/** @brief Queue a fan the routine did not draw when the view draws it: appended outright, or through the lateral test of a wider view.
 *
 * An accepted view verdict appends the fan; a lateral pyramid reject in a
 * view wider than 4:3 goes to glow_wide_admit. Every other verdict drops it.
 * @param v The tick's projection view.
 * @param r The record.
 * @param gv The verdict the view draws the record under.
 * @param wide Nonzero in a view wider than 4:3.
 * @param yaw The camera yaw the fan was built at.
 * @param f Its fan.
 * @return 1 when the fan was appended, else 0. */
static int glow_view_admit(const ngte_view* v, const glow_src* r, glow_verdict gv, int wide, int yaw,
                           const glow_fan* f) {
    if (f->n <= 0) return 0;
    if (gv == GLOW_V_ACCEPT) { glow_fan_append(r, yaw, f); return 1; }
    if (gv != GLOW_V_PYR_X || !wide) return 0;
    const int before = s_wide_admit;
    glow_wide_admit(v, r, yaw, f);
    return s_wide_admit != before;
}

/** @brief Build and queue one pool record's fan when the routine draws it or the view reveals it.
 *
 * A record the routine draws (glow_classify accepts it and glow_solo_skips
 * does not skip it) has its fan built at the tick's camera yaw and phase
 * (glow_fan_build) and appended, and counts into s_accepted. Any other
 * record is offered to glow_view_admit under the verdict the view draws it
 * under (glow_view_verdict): a record the routine rejected on its lateral
 * test in a view wider than 4:3, or one it rejected on its box or far test
 * under extended LOD, which counts into s_range_admit when appended. With
 * the cull log in range each live record past the box test prints a line;
 * with the census in range each accepted record prints its owner line
 * (glow_owner_line); with the log or the probe on, an accepted record's fan
 * at the routine's integer angle is kept in the context.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view.
 * @param s The pool slot.
 * @param c The capture's walk state. */
static void glow_capture_rec(CPUState* cpu, const ngte_view* v, uint32_t s, glow_capture_ctx* c) {
    glow_src r;
    const glow_verdict g = glow_classify(cpu, v, s, &r);
    if (g == GLOW_V_FREE || glow_solo_skips(cpu, r.packed)) return;
    const glow_verdict gv = glow_view_verdict(v, &r, g);
    const int accept = g == GLOW_V_ACCEPT;
    const int a = s_accepted < GLOW_REC_MAX ? s_accepted : GLOW_REC_MAX - 1;
    const char* view = "reject";
    glow_fan f = { .n = 0, .xmin = 0.0f, .xmax = 0.0f };
    c->fi[a].n = 0;
    if (gv == GLOW_V_ACCEPT || (c->wide && gv == GLOW_V_PYR_X))
        glow_fan_build(cpu, v, &r, glow_angle((double)c->yaw, (double)((r.packed >> 16) & 0xFFu)), &f);
    if (accept) {
        c->first[a] = s_item_n;
        c->vslot[a] = (int)s;
        if (c->log || c->vtx) glow_fan_build_int(cpu, v, &r, c->yaw, &c->fi[a]);
        glow_fan_append(&r, c->yaw, &f);
        if (glow_census_on() && s_item_n > c->first[a]) glow_owner_line(cpu, s, &r, &s_item[c->first[a]].src);
        view = "accept";
        s_accepted++;
    } else if (glow_view_admit(v, &r, gv, c->wide, c->yaw, &f)) {
        view = gv != g ? "range_admit" : "wide_admit";
        s_range_admit += gv != g;
    }
    if (c->log && g >= GLOW_V_NEAR) glow_cull_log_line(v, s, g, view, &r, &f, &c->fi[a], s_accepted - 1);
}

/** @brief Whether extended LOD feeds a native glow for a freed drawn-gated record, latched on first use.
 *
 * On by default; OPENPETE_GLOW_SYNTH with a value starting with '0' turns it
 * off, for an A/B from one binary. Only read while extended LOD is on.
 * @return 1 when on, else 0. */
static int glow_synth_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLOW_SYNTH"); v = !(e && *e == '0'); }
    return v;
}

#define GLOW_SIMPLE_MAX 1024   /* simple-queue entries one capture reads */

/* The mobys the simple models' queue of this tick holds, read once per
 * capture by glow_synth_set. */
static uint32_t s_simple[GLOW_SIMPLE_MAX];
static int      s_simple_n;

/** @brief Whether native draws a moby this tick: its animated record queue or its simple queue holds it.
 * @param cpu The guest CPU state, read only.
 * @param m The moby.
 * @return 1 when drawn, else 0. */
static int glow_native_draws(CPUState* cpu, uint32_t m) {
    for (int i = 0; i < s_simple_n; i++)
        if (s_simple[i] == m) return 1;
    return moby_rec_holds_moby(cpu, m);
}

/** @brief Whether a word is a main-RAM pointer.
 * @param a The word.
 * @return 1 when it lies in KSEG0 main RAM, else 0. */
static int glow_ram_ptr(uint32_t a) {
    return a >= 0x80000000u && a < 0x80200000u;
}

/* ---- the model-vertex glow position --------------------------------------
 *
 * Some classes place their glow record on vertices of their own animated
 * model (game_glow_synth_t.vtx_n). Each tick the update first rebuilds the
 * moby's rotation from its angle word (func_80055D24 with flag 4, which calls
 * func_8004EA90 on Moby+0x44 into Moby+0x20 from the identity, updatemobys.s
 * 0x80055E58-0x80055E78), then calls func_80055F14 per vertex
 * (updatemobys.s 0x80055F14-0x800560A8):
 *   * the keyframe pair and blend are the cull's: header A = anims[id],
 *     header B = anims[nextId] at nextFrame, blend = table[Moby+0x40]
 *     (moby_keyframe_pair); the shift is header A's byte 2.
 *   * vertex i of a keyframe is the word at header +0x20's data base plus
 *     (keyframe word & 0xFFFF) * 2 plus i * 4, read whole and split into
 *     bits 21-31, 11-21 and 0-11, each signed (0x80055F98-0x80055FE4); this
 *     is not the draw's vertex stream codec.
 *   * with a non-zero blend the GTE's INTPL takes A + (B - A) * blend * 16 /
 *     4096 per field (0x80056008-0x80056050); the fields are then shifted
 *     left by the shift.
 *   * the rotation times (bits 11-21, bits 0-11, bits 21-31) gives (m1, m2,
 *     m3) >> 12, and the vertex is (X + m3, Y - m1, Z - m2) from the moby's
 *     position (0x80056060-0x800560A8).
 * A two-vertex glow adds both world positions and halves each component
 * toward zero (func_8004F194, func_8004F110). This port takes every step in
 * float, without the INTPL's, the matrix's and the halving's truncations. */

/* The bytes of a moby the model-vertex position reads: the position words,
 * the class, the animation state and the angle word. */
#define GLOW_PART_BYTES (0x48u)

/** @brief One model vertex's three fields as func_80055F14 reads them from a keyframe.
 * @param cpu The guest CPU state, read only.
 * @param header The AnimationHeader.
 * @param word The keyframe's record word 0.
 * @param idx The vertex index.
 * @param f Receives the fields in the order the routine loads IR1, IR2 and IR3:
 *        bits 11-21, bits 0-11 and bits 21-31. */
static void glow_part_fields(CPUState* cpu, uint32_t header, uint32_t word, uint32_t idx, double f[3]) {
    const uint32_t base = cpu->read_word(header + 0x20u);
    const uint32_t w = cpu->read_word(base + ((word & 0xFFFFu) << 1) + idx * 4u);
    f[0] = (double)((int32_t)(w << 10) >> 21);
    f[1] = (double)((int32_t)(w << 20) >> 20);
    f[2] = (double)((int32_t)w >> 21);
}

/** @brief The world position of a model-vertex glow from one state of its moby, in float.
 *
 * The state is a copy of the moby's leading bytes, live or from an interp
 * snapshot; see the section comment for the computation.
 * @param cpu The guest CPU state, read only.
 * @param mb The moby's bytes from offset 0, at least GLOW_PART_BYTES.
 * @param k The class's entry; vtx_n is 1 or 2.
 * @param out Receives the position.
 * @return 0 when the class's model or the keyframe pair does not resolve, else 1. */
static int glow_part_pos(CPUState* cpu, const uint8_t* mb, const game_glow_synth_t* k, double out[3]) {
    uint16_t cls;
    memcpy(&cls, mb + game_layout.moby.class_off, 2);
    const uint32_t model = cpu->read_word(game_layout.moby.model_table + (uint32_t)cls * 4u);
    moby_keyframe_pair_t kp;
    if (!glow_ram_ptr(model) || !moby_keyframe_pair(cpu, model, mb + game_layout.moby.anim_state_off, &kp))
        return 0;
    const double scale = (double)(1u << (cpu->read_byte(kp.header + game_layout.moby.anim_shift_off) & 31u));
    const double t = (double)kp.blend * 16.0 / 4096.0;
    uint32_t rot;
    memcpy(&rot, mb + game_layout.moby.angle_off, 4);
    double ang[3];
    ngte_word_angles(rot, ang);
    static const float ident[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
    float R[3][3];
    ngte_compose_euler_mf(cpu, ident, ang, R);
    int32_t pos[3];
    memcpy(pos, mb + game_layout.moby.pos_off, 12);
    double sum[3] = { 0.0, 0.0, 0.0 };
    for (uint32_t n = 0; n < k->vtx_n && n < 2u; n++) {
        double a[3], b[3], ir[3];
        glow_part_fields(cpu, kp.header, kp.word_a, k->vtx[n], a);
        if (kp.blend) glow_part_fields(cpu, kp.header_b, kp.word_b, k->vtx[n], b);
        for (int i = 0; i < 3; i++) ir[i] = (kp.blend ? a[i] + (b[i] - a[i]) * t : a[i]) * scale;
        double m[3];
        for (int i = 0; i < 3; i++) m[i] = (double)R[i][0] * ir[0] + (double)R[i][1] * ir[1] + (double)R[i][2] * ir[2];
        sum[0] += (double)pos[0] + m[2];
        sum[1] += (double)pos[1] - m[0];
        sum[2] += (double)pos[2] - m[1];
    }
    for (int i = 0; i < 3; i++) out[i] = sum[i] / (double)k->vtx_n;
    return 1;
}

/** @brief The world position of a model-vertex glow from its moby's live state.
 * @param cpu The guest CPU state, read only.
 * @param m The moby.
 * @param k The class's entry.
 * @param out Receives the position.
 * @return glow_part_pos's verdict. */
static int glow_part_live(CPUState* cpu, uint32_t m, const game_glow_synth_t* k, double out[3]) {
    uint32_t w[GLOW_PART_BYTES / 4u];
    for (uint32_t i = 0; i < GLOW_PART_BYTES / 4u; i++) w[i] = cpu->read_word(m + i * 4u);
    return glow_part_pos(cpu, (const uint8_t*)w, k, out);
}

/** @brief Under extended LOD, feed the glow the game frees for one drawn-gated moby whose gating moby native draws.
 *
 * The game keeps the glow record only while the gating moby's drawn byte was
 * set (game_glow_synth_t): a child moby, or with child_cls 0 the moby itself;
 * extended LOD draws it where the game culls it. A moby whose tag holds a
 * live record is skipped (the pool walk feeds it), as is one whose child is
 * absent or of another class, or whose gating moby native does not draw this
 * tick (glow_native_draws). Otherwise the record is built as the game writes
 * it: the class's shape, size, colour and bias, at the child's position
 * moved by dpos or at the moby's model vertices (glow_part_live), and a phase
 * that is the gating moby's address bits 2-9, fixed while the glow is
 * synthesized. It is keyed by the gating moby's address, so a sub-tick
 * present moves it with that moby's own snapshots (glow_record_offset). It
 * passes the routine's tests without the box and far bounds (glow_cull), the
 * lateral one through glow_view_admit, and counts into s_synth_admit.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view.
 * @param m The moby.
 * @param k Its class's entry.
 * @param c The capture's walk state. */
static void glow_synth_moby(CPUState* cpu, const ngte_view* v, uint32_t m, const game_glow_synth_t* k,
                            const glow_capture_ctx* c) {
    const uint32_t tag = cpu->read_word(m);
    if (!glow_ram_ptr(tag) || cpu->read_word(tag + k->rec_off) != 0u) return;
    const uint32_t child = k->child_cls ? cpu->read_word(tag + k->child_off) : m;
    if (!glow_ram_ptr(child)) return;
    if (k->child_cls && cpu->read_half(child + game_layout.moby.class_off) != k->child_cls) return;
    if (!glow_native_draws(cpu, child)) return;
    glow_src r;
    memset(&r, 0, sizeof r);
    r.addr = child;
    r.synth = 1;
    r.part = k->vtx_n ? k : NULL;
    r.rgb = k->rgb & 0xFFFFFFu;
    r.packed = (uint32_t)k->shape | ((uint32_t)k->size << 8) | (((child >> 2) & 0xFFu) << 16) |
               ((uint32_t)(uint8_t)k->bias << 24);
    if (r.part) {
        if (!glow_part_live(cpu, m, k, r.w)) return;
    } else {
        for (uint32_t i = 0; i < 3u; i++)
            r.w[i] = (double)((int32_t)cpu->read_word(child + game_layout.moby.pos_off + 4u * i) + k->dpos[i]);
    }
    const glow_verdict gv = glow_cull(v, &r, 1);
    if (gv != GLOW_V_ACCEPT && gv != GLOW_V_PYR_X) return;
    glow_fan f;
    glow_fan_build(cpu, v, &r, glow_angle((double)c->yaw, (double)((r.packed >> 16) & 0xFFu)), &f);
    s_synth_admit += glow_view_admit(v, &r, gv, c->wide, c->yaw, &f);
}

/** @brief Under extended LOD, offer every live moby of a class in GW.synth to glow_synth_moby.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view.
 * @param c The capture's walk state. */
static void glow_synth_set(CPUState* cpu, const ngte_view* v, const glow_capture_ctx* c) OP_REQUIRES(sim) {
    if (!GW.synth_n || !openpete_no_lod || !glow_synth_enabled()) return;
    const uint32_t head = game_layout.moby.array_head;
    const uint32_t arr = game_layout.moby.array_direct ? head : cpu->read_word(head);
    if (!glow_ram_ptr(arr)) return;
    s_simple_n = moby_native_simple_mobys(s_simple, GLOW_SIMPLE_MAX);
    for (uint32_t i = 0; i < 1024u; i++) {
        const uint32_t m = arr + i * game_layout.moby.array_stride;
        const uint8_t st = cpu->read_byte(m + game_layout.moby.state_off);
        if (st == game_layout.moby.state_term) break;
        if (st & 0x80u) continue;                       /* dormant */
        const uint16_t cls = cpu->read_half(m + game_layout.moby.class_off);
        for (uint32_t j = 0; j < GW.synth_n; j++)
            if (GW.synth[j].cls == cls) glow_synth_moby(cpu, v, m, &GW.synth[j], c);
    }
}

/* ---- the model-vertex compare (OPENPETE_GLOW_PART_CMP) ---------------------
 *
 * The record's position words against glow_part_pos of the moby's live state
 * at the capture, for every live moby of a model-vertex class whose record
 * the class's update wrote this tick. The update writes it only when the
 * drawn byte it reads, the one the previous tick's draw left, is non-zero;
 * with that byte 0 an update that runs frees the record and one that does
 * not run (the moby outside its update radius) leaves a stale record, which
 * is counted as stale and not compared. Each error is the world distance and
 * the screen distance of the two projected points under the tick's view.
 * Level 2 adds a line per compared moby, level 3 one per moby without a
 * record. The summary prints at exit per class; a class with no compared
 * record reads VACUOUS. */

#define GLOW_CMP_CLS  4
#define GLOW_CMP_KEYS 64

typedef struct { glow_acc world, px; unsigned long stale; } glow_cmp_acc;
static glow_cmp_acc s_cmp[GLOW_CMP_CLS];
static struct { uint32_t moby; unsigned epoch; uint8_t drawn; } s_cmp_last[GLOW_CMP_KEYS];

/** @brief OPENPETE_GLOW_PART_CMP, latched on first use: 0 off, 1 the exit summary, 2 also per-moby lines, 3 also mobys without a record.
 * @return The level. */
static int glow_cmp_level(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLOW_PART_CMP"); v = (e && *e) ? atoi(e) : 0; }
    return v;
}

/** @brief Print the compare's per-class summary; registered with atexit. */
static void glow_cmp_report(void) {
    for (uint32_t j = 0; j < GW.synth_n && j < GLOW_CMP_CLS; j++) {
        if (!GW.synth[j].vtx_n) continue;
        const glow_cmp_acc* a = &s_cmp[j];
        fprintf(stderr, "[glow-part-cmp] cls=%03X compared=%lu stale=%lu world_mean=%.3f world_max=%.3f"
                        " px_n=%lu px_mean=%.4f px_max=%.4f verdict=%s\n",
                (unsigned)GW.synth[j].cls, a->world.n, a->stale,
                a->world.n ? a->world.sum / (double)a->world.n : 0.0, a->world.mx,
                a->px.n, a->px.n ? a->px.sum / (double)a->px.n : 0.0, a->px.mx,
                a->world.n ? "MEASURED" : "VACUOUS");
    }
}

/** @brief The screen point of a world position under a view, when it lies past the near plane.
 * @param v The view.
 * @param w The world position.
 * @param sx Receives the screen X.
 * @param sy Receives the screen Y.
 * @return 1 when projected, else 0. */
static int glow_cmp_screen(const ngte_view* v, const double w[3], double* sx, double* sy) {
    glow_src r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < 3; i++) r.w[i] = w[i];
    glow_cull(v, &r, 1);
    if (r.m3 < (double)GW.near_z) return 0;
    ngte_proj o;
    ngte_screen_gte(r.m1, r.m2, r.m3, v->ofx, v->ofy, v, 0, &o);
    *sx = o.sx; *sy = o.sy;
    return 1;
}

/** @brief The drawn byte a moby had at the previous capture, then store this capture's.
 * @param m The moby.
 * @param drawn This capture's drawn byte.
 * @return The previous capture's byte, or -1 when the store holds none from it. */
static int glow_cmp_swap_drawn(uint32_t m, uint8_t drawn) {
    int slot = -1, prev = -1;
    for (int i = 0; i < GLOW_CMP_KEYS && slot < 0; i++)
        if (s_cmp_last[i].moby == m) slot = i;
    for (int i = 0; i < GLOW_CMP_KEYS && slot < 0; i++)
        if (s_cmp_last[i].moby == 0u || s_cmp_last[i].epoch + 1u < s_epoch) slot = i;
    if (slot < 0) return -1;
    if (s_cmp_last[slot].moby == m && s_cmp_last[slot].epoch + 1u == s_epoch) prev = s_cmp_last[slot].drawn;
    s_cmp_last[slot].moby = m;
    s_cmp_last[slot].epoch = s_epoch;
    s_cmp_last[slot].drawn = drawn;
    return prev;
}

/** @brief Add one native position's error against the game's record to a class's accumulators.
 * @param a The class's accumulators.
 * @param v The tick's view.
 * @param game The record's position.
 * @param nat The native position.
 * @param px Receives the screen error, -1 when either point is behind the near plane.
 * @return The world error. */
static double glow_cmp_add(glow_cmp_acc* a, const ngte_view* v, const double game[3], const double nat[3],
                           double* px) {
    const double e = sqrt((game[0] - nat[0]) * (game[0] - nat[0]) + (game[1] - nat[1]) * (game[1] - nat[1]) +
                          (game[2] - nat[2]) * (game[2] - nat[2]));
    glow_acc_add(&a->world, e);
    double gx, gy, nx, ny;
    *px = -1.0;
    if (glow_cmp_screen(v, game, &gx, &gy) && glow_cmp_screen(v, nat, &nx, &ny)) {
        *px = hypot(gx - nx, gy - ny);
        glow_acc_add(&a->px, *px);
    }
    return e;
}

/** @brief Compare one moby's glow record, when its update wrote it this tick, with its native model-vertex position.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's view.
 * @param m The moby.
 * @param j The index of its class's entry in GW.synth. */
static void glow_cmp_moby(CPUState* cpu, const ngte_view* v, uint32_t m, uint32_t j) {
    const game_glow_synth_t* k = &GW.synth[j];
    const uint32_t tag = cpu->read_word(m);
    const uint8_t drawn = cpu->read_byte(m + game_layout.moby.drawn_off);
    const int pdrawn = glow_cmp_swap_drawn(m, drawn);
    double nat[3];
    if (!glow_ram_ptr(tag) || !glow_part_live(cpu, m, k, nat)) return;
    const double cd = sqrt((nat[0] - v->cam[0]) * (nat[0] - v->cam[0]) + (nat[1] - v->cam[1]) * (nat[1] - v->cam[1]) +
                           (nat[2] - v->cam[2]) * (nat[2] - v->cam[2]));
    const uint32_t rec = cpu->read_word(tag + k->rec_off);
    if (!glow_ram_ptr(rec) || (cpu->read_word(rec + GW.packed_off) & GLOW_SHAPE_MASK) == 0u) {
        if (glow_cmp_level() >= 3)
            fprintf(stderr, "[glow-part f=%u] moby=%08X cls=%03X drawn=%u native=%d no_record"
                            " live=(%.0f,%.0f,%.0f) cam_dist=%.0f\n",
                    openpete_frame, m, (unsigned)k->cls, drawn, glow_native_draws(cpu, m), nat[0], nat[1], nat[2], cd);
        return;
    }
    if (pdrawn <= 0) { s_cmp[j].stale += pdrawn == 0; return; }
    double game[3];
    for (uint32_t i = 0; i < 3u; i++) game[i] = (double)(int32_t)cpu->read_word(rec + GW.pos_off + 4u * i);
    double px;
    const double e = glow_cmp_add(&s_cmp[j], v, game, nat, &px);
    if (glow_cmp_level() >= 2)
        fprintf(stderr, "[glow-part f=%u] moby=%08X cls=%03X drawn=%u anim=%08X progress=%u game=(%.0f,%.0f,%.0f)"
                        " native=(%.2f,%.2f,%.2f) err=%.3f px=%.4f cam_dist=%.0f\n",
                openpete_frame, m, (unsigned)k->cls, drawn, cpu->read_word(m + game_layout.moby.anim_state_off),
                cpu->read_byte(m + game_layout.moby.anim_state_off + 4u), game[0], game[1], game[2],
                nat[0], nat[1], nat[2], e, px, cd);
}

/** @brief Under OPENPETE_GLOW_PART_CMP, compare every live moby of a model-vertex class with its game record.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's view. */
static void glow_cmp_set(CPUState* cpu, const ngte_view* v) {
    static int armed = 0;
    if (!glow_cmp_level() || !GW.synth_n) return;
    if (!armed) { armed = 1; atexit(glow_cmp_report); }
    const uint32_t head = game_layout.moby.array_head;
    const uint32_t arr = game_layout.moby.array_direct ? head : cpu->read_word(head);
    if (!glow_ram_ptr(arr)) return;
    for (uint32_t i = 0; i < 1024u; i++) {
        const uint32_t m = arr + i * game_layout.moby.array_stride;
        const uint8_t st = cpu->read_byte(m + game_layout.moby.state_off);
        if (st == game_layout.moby.state_term) break;
        if (st & 0x80u) continue;
        const uint16_t cls = cpu->read_half(m + game_layout.moby.class_off);
        for (uint32_t j = 0; j < GW.synth_n && j < GLOW_CMP_CLS; j++)
            if (GW.synth[j].cls == cls && GW.synth[j].vtx_n) glow_cmp_moby(cpu, v, m, j);
    }
}

/** @brief Calibrate every accepted record of one capture against the routine's fans, when the walked fans count them.
 *
 * Adds the capture's mean error per axis to the per-tick statistic, and
 * prints it at probe level 2.
 * @param c The capture's walk state. */
static void glow_vtx_cal_tick(const glow_capture_ctx* c) {
    if (!c->vtx || s_groups != s_accepted || s_accepted > GLOW_REC_MAX) return;
    double sum[2] = { 0.0, 0.0 };
    int n = 0;
    for (int a = 0; a < s_accepted; a++) glow_vtx_cal_rec(&c->fi[a], a, c->vslot[a], c->first[a], sum, &n);
    if (n == 0) return;
    glow_acc_add(&s_vtx_tick[0], sum[0] / n);
    glow_acc_add(&s_vtx_tick[1], sum[1] / n);
    if (glow_vtx_level() >= 2)
        fprintf(stderr, "[glow-vtx-cal f=%u] n=%d mean=(%.4f,%.4f)\n", openpete_frame, n,
                sum[0] / n, sum[1] / n);
}

/** @brief Build and queue the fan of every record the routine drew, of every record the view reveals, and of every glow extended LOD synthesizes.
 *
 * Walks the pool in slot order through glow_capture_rec, then the drawn-gated
 * classes through glow_synth_set, then calibrates the capture
 * (glow_vtx_cal_tick).
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view. */
static void glow_capture_set(CPUState* cpu, const ngte_view* v) OP_REQUIRES(sim) {
    static glow_capture_ctx c;
    c.wide = v->extended;
    c.log = glow_cull_log_on();
    c.vtx = glow_vtx_on();
    c.yaw = glow_cam_yaw(cpu);
    for (uint32_t s = 0; s < GW.slots; s++) glow_capture_rec(cpu, v, s, &c);
    glow_synth_set(cpu, v, &c);
    glow_cmp_set(cpu, v);
    glow_vtx_cal_tick(&c);
}

/** @brief Whether this feeder emits the glow fans, latched on first use.
 *
 * OPENPETE_GLOW with a value starting with '0' suppresses this feed alone.
 * The guest draw still runs, so the A/B prices the rings' painted pixels from
 * one binary.
 * @return 1 when enabled, else 0. */
static int glow_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLOW"); v = !(e && *e && *e == '0'); }
    return v;
}

/** @brief Add one fan triangle to the native scene as an untextured additive WORLD triangle.
 *
 * The fan is flat in view-Z, so any constant W interpolates its colour
 * screen-linearly: the item's depth serves as both. The vertex colours are
 * made opaque. A triangle whose record links at a bucket carries it in
 * ot_slot1. With `key` and a bucket above 0 it also carries the aura rank and
 * a koff whose slot is one bucket nearer than its own, which runs the aura
 * contest against the actor key image (psx_key_contest.glsl op_aura_loses):
 * the fan loses a pixel whose actor or player key lies in that slot or
 * nearer. The routine appends the fan at its bucket's tail (Spyro 2
 * mobyfunc.s:2141-2151), after the moby faces the moby draw, which the world
 * draw runs first (draw/func_800155A0.s:16, the glow routine at :30), spliced
 * into the same bucket at its tail (mobydraw.s 0x80045444-0x80045478), so a
 * face of the fan's own bucket is painted under it; the contest's own slot
 * is therefore the bucket less one. A fan at bucket 0 wins every face and
 * carries no contest. Counts the triangle in s_fed_tris.
 * @param it The triangle.
 * @param key Nonzero when the fan contests by its bucket (glow_key_on). */
static void glow_feed_tri(const glow_item* it, int key) OP_REQUIRES(sim) {
    const ngte_prim* p = &it->p;
    openpete_scene_tri st = {0};
    for (int k = 0; k < 3; k++) {
        st.v[k].sx = p->x[k]; st.v[k].sy = p->y[k];
        st.v[k].z = it->depth; st.v[k].pvz = it->depth;
        st.v[k].rgba = (p->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
    }
    st.blend = it->blend;
    openpete_scene_tri_set_bucket(&st, it->src.key, key);
    openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    s_fed_tris++;
}

/** @brief Push every queued triangle to the native scene at the positions this present shows.
 *
 * Does nothing when OPENPETE_GLOW suppresses the feed or nothing was
 * captured. Each triangle goes to the scene through glow_feed_tri, tagged as
 * a glow particle with sel 0, since a glow ring carries no game particle
 * class. A triangle of the companion's ring is skipped while the player is
 * hidden (player_hide_active). */
static void glow_feed_native(void) OP_REQUIRES(sim) {
    if (!glow_feed_enabled()) return;
    if (s_item_n <= 0) return;
    const int hide_player = player_hide_active();
    const int key = glow_key_on();
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_GLOW, 0, 0));
    for (int i = 0; i < s_item_n; i++) {
        const ngte_prim* p = &s_item[i].p;
        if (p->nv != 3) continue;
        const int sparx = s_item[i].sparx;
        player_hide_count_data(sparx ? PH_CAT_SPARX_GLOW : PH_CAT_OTHER_GLOW,
                               sparx && hide_player, p, sizeof *p);
        if (sparx && hide_player) continue;
        glow_feed_tri(&s_item[i], key);
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Mark every triangle whose record is the companion's glow record.
 *
 * The companion's record is the one its moby tag points at
 * (player_hide_sparx_glow).
 * @param cpu The guest CPU state, read only. */
static void glow_mark_sparx(CPUState* cpu) {
    const uint32_t rec = player_hide_sparx_glow(cpu);
    for (int i = 0; i < s_item_n; i++)
        s_item[i].sparx = rec != 0u && s_item[i].src.addr == rec;
}

/** @brief Print the per-call glow census when the VBL lies in the OPENPETE_GLOW_CENSUS range.
 *
 * OPENPETE_GLOW_CENSUS=LO-HI (or a single VBL): per call in that range, the
 * primitives the routine appended by GPU code, the fans its draw-mode packets
 * delimit, the records the pool walk says it drew, the fans the widened view
 * built and fed or culled, the box- or far-culled records extended LOD fed,
 * the native glows extended LOD fed for freed records, the triangles queued
 * and the native triangles this feeder emitted. fans != accepted names a call whose drawn set the pool walk
 * mispredicts. A capture whose fed count is zero measured nothing.
 * @param start First polybuf address of the routine's output.
 * @param end One past the last.
 * @param fed_before The fed triangle total before this call. */
static void glow_census(uint32_t start, uint32_t end, uint32_t fed_before) {
    if (!glow_census_on()) return;
    fprintf(stderr, "[glow-census f=%u] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, start, end, end - start, s_guest_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_guest_n; i++) seen[s_guest[i].code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    fprintf(stderr, " tris=%d fans=%d accepted=%d wide_admit=%d wide_cull=%d range_admit=%d"
                    " synth_admit=%d fed_tris=%u\n",
            s_item_n, s_groups, s_accepted, s_wide_admit, s_wide_cull, s_range_admit, s_synth_admit,
            s_fed_tris - fed_before);
}

/* ---- sub-tick present ---------------------------------------------------- */

/* A record's speed ceiling, in world units per tick. The keyed store is keyed
 * by slot address, so a slot a new record respawns into inside one tick pairs
 * across two different rings and would lerp as a streak between them. The
 * packed-word test below catches a reuse that changed the shape, the size or
 * the ordering bias; this catches one that kept all three, at a distance no
 * record covers in a tick. */
#define GLOW_REUSE_CEILING 4096.0

/* The packed word's stable bytes: the shape index, the size and the signed
 * ordering-table bias, all written once by the spawner. Byte 2 is the spin
 * phase, which the routine rewrites on every draw, so it is masked out of the
 * identity test. */
#define GLOW_PACKED_STABLE 0xFF00FFFFu

static unsigned long s_reb_stat[2];   /* 0 lerped, 1 snapped */
static double        s_reb_worldmax;  /* the largest per-tick world step paired */

/** @brief The byte offset of a record's position words in the snapshot its key names: the pool record's, or for a synthesized glow its child moby's.
 * @param r The record.
 * @return The offset. */
static uint32_t glow_rec_pos_off(const glow_rec* r) {
    return r->synth ? game_layout.moby.pos_off : GW.pos_off;
}

/** @brief Whether two snapshots of a record's key hold the same glow: the pool record's GLOW_PACKED_STABLE bytes agree; a synthesized glow's key is its child moby, which carries no packed word.
 * @param r The record.
 * @param o The key's snapshots.
 * @param dphase Receives the phase change toward the previous snapshot, unscaled.
 * @return 1 when the pair is the same glow, else 0. */
static int glow_rec_same(const glow_rec* r, const openpete_interp_obj_t* o, double* dphase) {
    *dphase = 0.0;
    if (r->synth) return 1;
    if ((uint32_t)o->blob_len < GW.packed_off + 4u) return 0;
    uint32_t pp, cp;
    memcpy(&pp, o->prev + GW.packed_off, 4);
    memcpy(&cp, o->curr + GW.packed_off, 4);
    if ((pp & GLOW_PACKED_STABLE) != (cp & GLOW_PACKED_STABLE)) return 0;
    *dphase = (double)(int8_t)(uint8_t)(((pp >> 16) - (cp >> 16)) & 0xFFu);
    return 1;
}

/** @brief Compute a record's world displacement and phase change between the pose the tick drew and the pose this present shows.
 *
 * Reads the previous snapshot of the record's key from the interp object
 * table and blends toward it by 1 - alpha. The phase change is the previous
 * snapshot's phase byte minus the current snapshot's, as a signed byte, so it
 * is the routine's per-draw step whichever side of the draw the snapshots are
 * taken; a synthesized glow's phase is fixed. A model-vertex glow steps by
 * its position from each snapshot of its moby (glow_part_pos), so it follows
 * the moby's animation as well as its motion. Returns 0 with a zero offset
 * and phase change for a record with no previous snapshot, a snapshot blob
 * too short for the position words, a pair whose prev was not observed up
 * to curr (cut_feeder_pair_is_tick), a pair glow_rec_same rejects, or a
 * displacement beyond GLOW_REUSE_CEILING.
 * @param cpu The guest CPU state, read only.
 * @param r The record as the tick read it.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @param off Receives the world-unit offset from the tick's position.
 * @param dphase Receives the phase change toward the previous tick, scaled by 1 - alpha.
 * @return 1 when the previous snapshot is the same record, else 0. */
static int glow_record_offset(CPUState* cpu, const glow_rec* r, double alpha, double off[3],
                              double* dphase) {
    off[0] = off[1] = off[2] = 0.0;
    *dphase = 0.0;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(r->addr);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    const uint32_t po = glow_rec_pos_off(r);
    if ((uint32_t)o->blob_len < (r->part ? GLOW_PART_BYTES : po + 12u)) return 0;
    if (!cut_feeder_pair_is_tick("glow", r->addr, o, r->part ? 0u : po,
                                 r->part ? GLOW_PART_BYTES : 12u)) return 0;
    double step;
    if (!glow_rec_same(r, o, &step)) return 0;
    double pw[3], cw[3];   /* a synthesized glow steps by its moby's own pair */
    if (r->part) {
        if (!glow_part_pos(cpu, o->prev, r->part, pw) || !glow_part_pos(cpu, o->curr, r->part, cw)) return 0;
    } else {
        int32_t pi[3], ci[3];
        memcpy(pi, o->prev + po, 12);
        memcpy(ci, o->curr + po, 12);
        for (int k = 0; k < 3; k++) { pw[k] = (double)pi[k]; cw[k] = (double)ci[k]; }
    }
    double d[3], sq = 0.0;
    for (int k = 0; k < 3; k++) {
        d[k] = pw[k] - (r->synth ? cw[k] : r->w[k]);
        sq += d[k] * d[k];
    }
    if (sq > GLOW_REUSE_CEILING * GLOW_REUSE_CEILING) return 0;
    if (sqrt(sq) > s_reb_worldmax) s_reb_worldmax = sqrt(sq);
    const double rest = 1.0 - alpha;
    for (int k = 0; k < 3; k++) off[k] = d[k] * rest;
    *dphase = step * rest;
    return 1;
}

/** @brief The camera yaw halfword of the earlier snapshot of the pair this present lies between.
 *
 * The interp ring's camera row runs on to the yaw (interp_regions_spyro23.c),
 * so the yaw is blended between the same two snapshots the staged camera
 * matrix and position are.
 * @param out Receives the yaw, sign-extended.
 * @return 1 when the pair and the camera row exist, else 0. */
static int glow_prev_cam_yaw(double* out) {
    static int region = -2;
    if (region == -2) region = game_interp_row_index(GAME_INTERP_CAM_ROW);
    if (region < 0 || !GW.cam_yaw_off) return 0;
    double qa = 0.0;
    const openpete_interp_snapshot_t *pv = NULL, *cu = NULL;
    if (!openpete_interp_query(openpete_interp_present_now(), g_openpete_tick_period_ns,
                               &qa, &pv, &cu) || !pv) return 0;
    int16_t y;
    memcpy(&y, pv->data + pv->region_offsets[region] + GW.cam_yaw_off, 2);
    *out = (double)y;
    return 1;
}

/** @brief The record and the angle a present shows a captured record at, under the present's view.
 *
 * The position and the phase are the record's own, blended toward its
 * previous snapshot by 1 - alpha (glow_record_offset); the camera yaw is the
 * captured one blended toward the previous snapshot's by 1 - alpha along the
 * shorter way round the 4096-unit circle. At alpha 1 every blend term is zero
 * and the record and angle are the capture's.
 * @param cpu The guest CPU state, read only.
 * @param v The present's view.
 * @param rec The record as the tick read it.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @param pyaw The previous snapshot's camera yaw, or NULL to hold the captured yaw.
 * @param r Receives the record with its view-space centre under `v`.
 * @param ang Receives the angle in sine-table entries.
 * @return 1 when the record was paired with its previous snapshot, else 0. */
static int glow_shown(CPUState* cpu, const ngte_view* v, const glow_rec* rec, double alpha,
                      const double* pyaw, glow_src* r, double* ang) {
    double off[3], dphase;
    const int paired = glow_record_offset(cpu, rec, alpha, off, &dphase);
    r->addr = rec->addr;
    r->packed = rec->packed;
    r->rgb = rec->rgb;
    r->synth = rec->synth;
    r->part = rec->part;
    for (int k = 0; k < 3; k++) r->w[k] = rec->w[k];
    const double p[3] = { (double)rec->w[0] + off[0], (double)rec->w[1] + off[1],
                          (double)rec->w[2] + off[2] };
    r->off[0] = p[0] - v->cam[0];
    r->off[1] = v->cam[1] - p[1];
    r->off[2] = v->cam[2] - p[2];
    ngte_mac(v->cam_M, r->off[1], r->off[2], r->off[0], &r->m1, &r->m2, &r->m3);
    double yaw = (double)rec->yaw;
    if (pyaw) {
        const double d = fmod(*pyaw - yaw + 2048.0 + 8192.0, 4096.0) - 2048.0;
        yaw += d * (1.0 - alpha);
    }
    *ang = glow_angle(yaw, (double)((rec->packed >> 16) & 0xFFu) + dphase);
    return paired;
}

/** @brief Print the rebuild's pairing counts and largest paired step for the run, registered at exit.
 *
 * Registered by glow_rebuild_all when OPENPETE_GLOW_REBASE_DBG is set,
 * non-empty and not starting with '0'. A run whose lerped count is zero
 * blended nothing toward a previous record and measured nothing. */
static void glow_rebase_report(void) {
    OP_ROLE_ASSERT(sim);
    fprintf(stderr, "[glow-rebase] lerped=%lu snapped=%lu worldmax=%.3f\n",
            s_reb_stat[0], s_reb_stat[1], s_reb_worldmax);
}

/** @brief Latch OPENPETE_GLOW_REBASE_DBG and register the pairing report at exit when it is set, non-empty and not starting with '0'. */
static void glow_rebase_dbg_arm(void) {
    static int dbg = -1;
    if (dbg >= 0) return;
    const char* e = getenv("OPENPETE_GLOW_REBASE_DBG");
    dbg = (e && *e && *e != '0');
    if (dbg) atexit(glow_rebase_report);
}

/** @brief Build the fan a present shows one captured record at, under the present's view.
 *
 * The record, phase and yaw come from glow_shown; a shown centre below the
 * routine's near plane builds no fan. The fan's depth is glow_record_depth of
 * the shown centre's view Z under the present's view, so the fan meets the
 * terrain at the depth this present shows it at; at alpha 1 the view and the
 * record are the tick's and the depth is the capture's. The ordering-table
 * key the fan contests moby faces by stays the tick's (glow_feed_tri).
 * @param cpu The guest CPU state, read only.
 * @param v The present's view.
 * @param rec The record as the tick read it.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @param pyaw The previous snapshot's camera yaw, or NULL to hold the captured yaw.
 * @param f Receives the fan; f->n = 0 when none was built.
 * @param paired Receives glow_shown's pairing verdict; may be NULL.
 * @return 1 when the fan was built, 0 past the near plane. */
static int glow_fan_at(CPUState* cpu, const ngte_view* v, const glow_rec* rec, double alpha,
                       const double* pyaw, glow_fan* f, int* paired) {
    glow_src r;
    double ang;
    const int pr = glow_shown(cpu, v, rec, alpha, pyaw, &r, &ang);
    if (paired) *paired = pr;
    f->n = 0;
    if (r.m3 < (double)GW.near_z) return 0;
    glow_fan_build(cpu, v, &r, ang, f);
    int key;
    f->depth = glow_record_depth(r.m3, glow_bias(r.packed), &key);
    return 1;
}

/** @brief Set one triangle's fed positions and depth from a fan, or from its capture when the fan lacks it.
 * @param it The triangle.
 * @param f The fan its record shows at this present, or NULL. */
static void glow_item_place(glow_item* it, const glow_fan* f) {
    const int use = f && it->tri < f->n;
    it->depth = (use && !sac_tick_depth(0)) ? f->depth : it->src.depth;
    for (int k = 0; k < 3; k++) {
        it->p.x[k] = use ? f->x[it->tri][k] : it->cx[k];
        it->p.y[k] = use ? f->y[it->tri][k] : it->cy[k];
    }
}

/** @brief Hand one record's fed centre to the sub-tick anchor probe with the keyed store's pair.
 *
 * The previous point is the store's previous snapshot when its pair spans
 * one tick (cut_pair_is_tick), else none; the current point is the
 * position the tick drew. A store current snapshot whose position differs
 * from the drawn one is printed at probe level 2.
 * @param rec The record as the tick read it.
 * @param f The fan built for this present.
 * @param v The present's view. */
static void glow_sac(const glow_rec* rec, const glow_fan* f, const ngte_view* v) {
    const double cw[3] = { rec->w[0], rec->w[1], rec->w[2] };
    const openpete_interp_obj_t* o = rec->synth ? NULL : openpete_interp_objs_find(rec->addr);
    if (!o || !o->have_prev || o->curr_ns <= o->prev_ns ||
        !cut_pair_is_tick(o) ||
        (uint32_t)o->blob_len < GW.pos_off + 12u) {
        sac_judge(SAC_GLOW, rec->addr, NULL, cw, 0, 0, f->csx, f->csy, v);
        return;
    }
    int32_t pw[3], cc[3];
    memcpy(pw, o->prev + GW.pos_off, 12);
    memcpy(cc, o->curr + GW.pos_off, 12);
    if ((cc[0] != rec->w[0] || cc[1] != rec->w[1] || cc[2] != rec->w[2]) && sac_level() >= 2)
        fprintf(stderr, "[sac-glow-currmismatch vbl=%u] rec=%08X drawn=(%.0f,%.0f,%.0f) store=(%d,%d,%d)\n",
                openpete_frame, rec->addr, rec->w[0], rec->w[1], rec->w[2], cc[0], cc[1], cc[2]);
    const double p[3] = { pw[0], pw[1], pw[2] };
    sac_judge(SAC_GLOW, rec->addr, p, cw, (int64_t)o->prev_ns, (int64_t)o->curr_ns, f->csx, f->csy, v);
}

/** @brief Hand one record's depth on this present to the sub-tick anchor probe: the capture's, the fed and the true one.
 * @param it The record's first triangle, placed for this present.
 * @param f The fan built for this present.
 * @param alpha The present's alpha. */
static void glow_sac_depth(const glow_item* it, const glow_fan* f, double alpha) {
    sac_depth(SAC_GLOW, it->src.depth, it->depth, f->depth, alpha);
}

/** @brief Rebuild every queued fan at the state this present shows, under the staged camera.
 *
 * The staged camera resolves through the sub-tick view, so building a view
 * here reads the interpolated transform the rest of the pass draws under.
 * Each record's fan is built once (glow_fan_at) and written into its
 * triangles; a record whose shown centre falls below the routine's near
 * plane, or whose fan has fewer triangles than were captured, keeps the
 * positions built at capture.
 * @param cpu The guest CPU state, read only.
 * @param alpha The present's position in the tick interval, 1 at its end. */
static void glow_rebuild_all(CPUState* cpu, double alpha) {
    if (s_item_n <= 0) return;
    glow_rebase_dbg_arm();
    ngte_view v;
    ngte_view_build(cpu, &v);
    double pyaw;
    const double* yp = glow_prev_cam_yaw(&pyaw) ? &pyaw : NULL;
    static glow_fan f;
    int ok = 0;
    for (int i = 0; i < s_item_n; i++) {
        glow_item* it = &s_item[i];
        if (it->tri == 0) {
            int paired;
            ok = glow_fan_at(cpu, &v, &it->src, alpha, yp, &f, &paired);
            s_reb_stat[paired ? 0 : 1]++;
            if (ok && sac_level() > 0) glow_sac(&it->src, &f, &v);
        }
        glow_item_place(it, ok ? &f : NULL);
        if (ok && it->tri == 0 && sac_level() > 0) glow_sac_depth(it, &f, alpha);
    }
}

/** @brief Restore every queued triangle to the positions built at capture. */
static void glow_restore_capture(void) {
    for (int i = 0; i < s_item_n; i++) glow_item_place(&s_item[i], NULL);
}

/** @brief Add one corner of one present to the probe's place, jump and identity statistics.
 * @param e The corner's key.
 * @param fed The fed corner and the displaced-routine corner, [form][axis].
 * @param model The model corner.
 * @param has_old Nonzero when the triangle carries the routine's corners.
 * @param leg 0 for the canonical present, 1 for a sub-tick one.
 * @param alpha The sub-tick present's alpha. */
static void glow_vtx_corner(glow_vkey* e, const float fed[GV_N][2], const float model[2],
                            int has_old, int leg, double alpha) {
    for (int f = 0; f < GV_N; f++) {
        if (f == GV_OLD && !has_old) continue;
        glow_acc_add(&s_vtx_place[f][leg],
                     hypot((double)fed[f][0] - model[0], (double)fed[f][1] - model[1]));
        if (leg == 1 && alpha <= 0.0 && e->a1_epoch + 1u == s_epoch)
            glow_acc_add(&s_vtx_jump[f], hypot((double)fed[f][0] - e->a1[f][0],
                                               (double)fed[f][1] - e->a1[f][1]));
        if (leg == 1 && alpha >= 1.0 && e->canon_epoch == s_epoch)
            glow_acc_add(&s_vtx_ident[f], hypot((double)fed[f][0] - e->canon[f][0],
                                                (double)fed[f][1] - e->canon[f][1]));
    }
    if (leg == 0) { e->canon_epoch = s_epoch; memcpy(e->canon, fed, sizeof e->canon); }
    if (leg == 1 && alpha >= 1.0) { e->a1_epoch = s_epoch; memcpy(e->a1, fed, sizeof e->a1); }
}

/** @brief Add one triangle of one present to the probe.
 *
 * The displaced-routine corner is the routine's corner moved by the record
 * centre's screen motion from the capture to (sx, sy).
 * @param it The triangle.
 * @param f The model fan of its record at this present.
 * @param sx The record centre's screen X at this present.
 * @param sy The record centre's screen Y at this present.
 * @param leg 0 for the canonical present, 1 for a sub-tick one.
 * @param alpha The sub-tick present's alpha. */
static void glow_vtx_tri(const glow_item* it, const glow_fan* f, double sx, double sy, int leg,
                         double alpha) {
    const int slot = glow_vslot(it->src.addr);
    if (slot < 0 || it->tri >= f->n || it->tri >= GLOW_TRI_N) return;
    for (int k = 0; k < 3; k++) {
        const float fed[GV_N][2] = {
            { it->p.x[k], it->p.y[k] },
            { it->gx[k] + (float)(sx - it->csx), it->gy[k] + (float)(sy - it->csy) },
        };
        const float model[2] = { f->x[it->tri][k], f->y[it->tri][k] };
        glow_vtx_corner(&s_vkey[slot][it->tri][k], fed, model, it->has_g, leg, alpha);
    }
}

/** @brief Measure one present's fed corners, and the routine's displaced corners, against the fan built at the shown state.
 *
 * The model is the fan glow_fan_at builds for this present's alpha (1 on the
 * canonical present) under the present's view, built here apart from the
 * feed. The displaced-routine form moves the routine's corners by their
 * record centre's screen motion between the capture and this present, held at
 * zero past the near plane. Counts the sub-tick presents whose previous
 * camera yaw differs from the first record's captured yaw.
 * @param cpu The guest CPU state, read only.
 * @param leg 0 for the canonical present, 1 for a sub-tick one.
 * @param alpha The sub-tick present's alpha. */
static void glow_vtx_present(CPUState* cpu, int leg, double alpha) {
    if (!glow_vtx_on() || s_item_n <= 0) return;
    ngte_view v;
    ngte_view_build(cpu, &v);
    double pyaw = 0.0;
    const double* yp = (leg == 1 && glow_prev_cam_yaw(&pyaw)) ? &pyaw : NULL;
    s_vtx_yaw_cmp++;
    if (yp && pyaw != (double)s_item[0].src.yaw) s_vtx_yaw_moved++;
    static glow_fan f;
    double sx = 0.0, sy = 0.0;
    for (int i = 0; i < s_item_n; i++) {
        const glow_item* it = &s_item[i];
        if (it->tri == 0) {
            const int ok = glow_fan_at(cpu, &v, &it->src, leg ? alpha : 1.0, yp, &f, NULL);
            sx = ok ? f.csx : it->csx;
            sy = ok ? f.csy : it->csy;
        }
        glow_vtx_tri(it, &f, sx, sy, leg, alpha);
    }
}

/* ---- hook ---------------------------------------------------------------- */

static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_GLOW, .prof_tag = "glow.srend", .st = &s_hb,
};

/** @brief Dispatch hook on the glow routine: run it once, then build and feed the fans of the records it drew.
 *
 * Runs through the shared bracket. Refuses (returns 0, the ordinary dispatch runs) a call whose return
 * address differs from GW.ra_guard when the layout names one. A taken call
 * empties the queued set, retiring the previous epoch's set first when it
 * is the epoch's first call (cut_held_sets_capture). When the routine
 * appended anything, or the view is wider than 4:3, or extended LOD is on,
 * or the cull log is on, the pool is walked under the tick's view and every
 * drawn record's fan is built (glow_capture_set). The routine's primitives are walked only for the
 * census, the cull log and the vertex probe. The set is fed natively when it
 * holds anything, the scene is live and RP_GLOW is selected; a non-empty set
 * from a call that appended nothing marks RP_GLOW drawn
 * (render_path_mark_drawn), since only a polybuf interval or the mark makes
 * the sub-tick re-extract feed the set. The census runs on every taken call.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the call is refused or the bracket declines, else 1 (the
 *         routine has already run). */
static int on_glow_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    /* A game whose image holds a second call site over a frozen frame names
     * the world draw's return address; every other caller is refused. */
    if (GW.ra_guard && cpu->ra != GW.ra_guard) return 0;
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    cut_held_sets_capture(&s_sets);
    s_item_n = 0; s_guest_n = 0; s_groups = 0; s_accepted = 0; s_wide_admit = 0; s_wide_cull = 0;
    s_range_admit = 0; s_synth_admit = 0;
    s_epoch++;
    const uint32_t fed_before = s_fed_tris;
    const int log = glow_cull_log_on();
    if (endp > startp && (glow_census_on() || log || glow_vtx_on()))
        glow_capture_guest(cpu, startp, endp);
    /* A view wider than 4:3, or extended LOD, can show a fan the routine
     * emitted nothing for, so either walks the pool even after an empty call. */
    if (endp > startp || ngte_view_extended(cpu) || openpete_no_lod || log) {
        ngte_view view;
        ngte_view_build(cpu, &view);
        glow_capture_set(cpu, &view);
        if (s_item_n > 0) {
            glow_mark_sparx(cpu);
            render_material_level_push(cpu);
            if (feed && render_path_enabled(RP_GLOW)) {
                glow_feed_native();
                glow_vtx_present(cpu, 0, 1.0);
            }
            /* A set of fans the routine emitted nothing for records no
             * polybuf interval; the mark keeps the sub-tick re-extract
             * feeding it. */
            if (endp <= startp) render_path_mark_drawn(RP_GLOW);
        }
    }
    glow_census(startp, endp, fed_before);
    return hook_bracket_end(&k_bracket);
}

/* Sub-tick re-extract; see glow.h. */
void glow_reextract(CPUState* cpu, int bracketed, double alpha, int hold) OP_REQUIRES(sim) {
    const int pick = cut_held_sets_enter(&s_sets, hold);
    if (hook_reextract_ready_held(RP_GLOW, pick)) {
        if (pick >= 0) {
            /* The staged camera is tick N-1's, the camera this set was built
             * under: the fans stand where that tick built them. */
            glow_restore_capture();
        } else if (bracketed) {
            sac_present_begin(SAC_GLOW, alpha);
            glow_rebuild_all(cpu, alpha);
            glow_vtx_present(cpu, 1, alpha);
        } else if (s_item_n > 0 && sac_level() > 0) {
            sac_unbracketed_present(SAC_GLOW);
            for (int i = 0; i < s_item_n; i++)
                if (s_item[i].tri == 0)
                    sac_unbracketed_anchor(SAC_GLOW, hypot((double)s_item[i].p.x[0] - s_item[i].cx[0],
                                                           (double)s_item[i].p.y[0] - s_item[i].cy[0]));
        }
        render_material_level_push(cpu);
        glow_feed_native();
    }
    cut_held_sets_leave(&s_sets, pick);
}

/* The hook address is the layout's glow draw site, bound at boot: a static
 * initializer cannot read another TU's const. A game with no located routine
 * leaves it 0 and the registry dispatches nothing. */
static feature_dispatch_entry_t glow_dispatch[] = {
    { 0u, on_glow_draw, 0u },
    { 0u, NULL, 0u },
};

/** @brief Bind the glow dispatch entry to the layout's routine address.
 *
 * A game whose layout locates no routine leaves the address 0.
 * @param opts Unused. */
static void glow_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    glow_dispatch[0].addr = game_layout.glow.hook;
}

const feature_module_t glow_module = {
    .name           = "glow_native",
    .apply_options  = glow_apply_options,
    .dispatch_table = glow_dispatch,
};
