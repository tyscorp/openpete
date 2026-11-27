// games/common/renderer/glint.c - the gem-glint feeder: a handwritten routine
// (Spyro 2 func_8004AC78 / mobyfunc.s:2192-2489, Spyro 3 func_8002F098 /
// mobyfunc.s:2195-2507; the same 297 instructions but for constants and
// symbols) that draws one screen-facing four-point star per live record of a
// pool and links each star into the 3D ordering table itself, so none of its
// primitives reaches the 2D chokepoint the sprite feeder hooks. Every per-game
// address, count, stride and bound comes from game_layout.glint; the walk
// below is one body.
//
// Read from the routine (asm/mobyfunc.s):
//   * the pool is 16 records of 0x10 bytes: world XYZ at +0x00, +0x04, +0x08
//     and a packed word at +0x0C holding the remaining lifetime in byte 0, the
//     size in byte 1, a spin phase in byte 2 and a colour-table row in byte 3
//     (:2231-2247).
//   * the routine subtracts the frame's elapsed count from the lifetime byte and
//     stores it back, dropping the record when it reaches zero (:2236-2240), so
//     the post-call lifetime byte is exactly the accept verdict for this pass.
//   * the record's camera-relative offset is halved before the rotation
//     (:2252-2258), so every view-space quantity the routine computes, the
//     near/far threshold included, is in half-world units.
//   * three cull stages: the halved offset must stay inside a +/-0x7800 box
//     (:2266-2276), the rotated depth must be at least 0x80 and under 0x7800
//     (:2294-2296), and the rotated X and Y must stay inside the view pyramid
//     4|X| - 3Z < 0x100 and 3|Y| - Z < 0xC0 (:2310, :2320).
//   * the size is a triangular ramp on the remaining lifetime times the size
//     byte, and the spin phase indexes the sine table for a 2x2 rotation-scale
//     basis loaded into the GTE rotation registers (:2324-2330), so the star
//     always faces the screen.
//   * the colour-table row is three words, each carrying 0x3A in its top byte:
//     the pale centre, the spike tip and the near-black waist, in that order
//     (:2330).
//   * under half-world depth 0x2000 the routine takes its near branch
//     (:2347-2348): one bundled draw-mode mini-packet (0xE1000620, whose ABR
//     bits are 0b01, additive) at :2383, then eight POLY_G4 semi-transparent
//     quads at :2387-2417, each spanning one rim segment of a seventeen-point
//     outline (a centre plus sixteen rim points, four spikes at radius 512 and
//     three shoulder points between each pair) that the GTE transformed into
//     the scratchpad at :2366-2373. Waist corners take the dark word, the spike
//     corner the tip word and the centre corner the pale word, so the star is
//     bright at its middle and fades out along each spike.
//   * at or above that depth the far branch (:2432) emits two opaque LINE_F2
//     instead, a plain cross through four projected endpoints, colouring them
//     with the pale word raised by 0x06000000 so its 0x3A code byte becomes
//     0x40 (:2480-2487).
//   * the ordering-table bucket is (Z >> 6) - 6 entries of eight bytes off the
//     base, clamped at zero (:2338-2344), with the far branch pushed a further
//     0x100 bytes back above depth 0x4000 (:2435).
//
// Which records drew in which branch is the routine's own verdict, read off the
// primitives it appended; where each star stands is not. The routine floors
// its halved camera offset, its rotated MACs, its size-scaled sine basis and
// every projected corner, so each corner lands up to a pixel off a position
// that changes every tick while the gem spins in place. Each primitive's
// corners are therefore rebuilt in float from its pool record, the camera and
// the routine's own outline, quad and colour tables (glint_corners), and the
// polybuf corners serve only to calibrate that rebuild. The primitives carry
// no depth either - the PSX orders them by ordering-table bucket - so each
// star's view-Z comes from its own pool record and the moby that owns it
// (glint_star_vz), paired to the star
// through the routine's own bookkeeping: a near record emits exactly one draw-mode packet and a far record
// emits exactly two lines, both in pool order, and the record walk here mirrors
// the routine's own rejects to say which records were accepted in which mode.
// The pairing is used only when the predicted near and far counts both agree
// with the captured ones; otherwise every primitive of the call is left unpaired
// and the census reports it.
//
// The pairing also carries what a sub-tick present needs: the record's
// address, its position words, its spin phase and lifetime, and its own
// projected screen point. A replay blends the record's position, lifetime and
// phase toward the previous tick's at the present's alpha and rebuilds the
// corners there under the staged camera.
//
// Spyro 1's sparkle renderer is fed by a float re-derivation in
// games/spyro-1/renderer/particle.c over a different record shape (a 0x18-byte
// record carrying a speed, a fade and a style byte) and a different star (a
// nine-point outline synthesized from the stock cross endpoints), with its
// primitives serving only as its A/B oracle. What the two share - the hook
// bracket, the polybuf walk, the primitive decode and the projection helpers -
// is already shared and is reused here unchanged.
//
// A record sits inside the gem or orb that placed it: a gem handler puts its
// near record at the gem's origin plus the gem's rotation times one point of
// the game's sparkle table, its far record at the origin plus 0x80 Z, and an
// orb copies its origin into the record. At the record's own depth the star
// is cut by its owner's front faces. Each record therefore takes an owner -
// the nearest moby the canonical pass drew whose origin lies within the
// sparkle table's reach of the record - and the star's depth is the nearer of
// the record's view-Z and the view-Z of the owner's origin less the owner's
// model radius, the largest distance of any vertex the native draw emits for
// it. The owner's faces then never hide its star, while a surface nearer than
// the owner's bounding sphere still does. A record with no owner keeps its
// own depth.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the lifetime byte it
// decrements in each record; this feeder only reads them afterwards.

#include "openpete_fiber.h"  /* openpete_feed_key_now: instrument line labels */
#include "glint.h"
#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "renderer/cut_guard.h"     /* cut_held_sets_t: the held cut's tick N-1 set */
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_interp_objs.h"   /* the per-record prev/curr pool snapshots */
#include "game_layout.h"
#include "no_lod.h"                 /* openpete_no_lod: extended LOD draws every record as a star */
#include "renderer/moby.h"          /* the drawn moby sets a record's owner is searched in */
#include "renderer/moby_game.h"     /* moby_game.model_is_animated: the owner's model kind */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

extern unsigned openpete_frame;

#define GT (game_layout.glint)

#define GLINT_MAX_PRIMS 256
#define GLINT_DRMODE    0xE1u   /* GP0 draw-mode packet, one per accepted near record */
#define GLINT_QUAD      0x3Au   /* POLY_G4 semi-transparent, eight per near record */
#define GLINT_LINE      0x40u   /* LINE_F2 opaque, two per accepted far record */
#define GLINT_QUADS_PER_STAR 8
#define GLINT_LINES_PER_CROSS 2
#define GLINT_LATERAL   2       /* glint_accept: rejected on the lateral test */
#define GLINT_RANGE     4       /* glint_accept: rejected on the box or the far depth */

/* One pool record the walk accepted: what the pairing hands its primitives
 * and what the sub-tick rebase re-projects it from. `addr` is the record's
 * address, which is its slot and stays its identity across ticks. */
typedef struct {
    float    depth;      /* the scene depth its primitives take */
    int      near_mode;  /* 1 = the near star branch, 0 = the far cross */
    uint32_t addr;       /* 0 on a primitive the pairing left unowned */
    uint32_t w[3];       /* its position words as the tick read them */
    uint8_t  life;       /* its lifetime byte after the routine's decrement */
    uint8_t  spin;       /* its spin-phase byte */
    uint8_t  size;       /* its size byte */
    uint8_t  row;        /* its colour-table row */
    double   psx, psy;   /* its own projected screen point, from the unfloored offset */
    double   gsx, gsy;   /* that point under the GTE's own H (no --fov scale), where the routine drew it */
    double   pvz;        /* that point's half-world view-Z */
    double   own[3];     /* world vector from the record to its owner's origin */
    float    own_rad;    /* the owner's model radius in world units; 0 = no owner */
    float    pull;       /* world units the owner's sphere front pulled the depth nearer */
} glint_rec;

typedef struct {
    ngte_prim p;
    float     depth;
    int       paired;
    int       blend;
    int       sub;        /* the quad (0-7) or line (0-1) of its record it is */
    int       exact;      /* 1 = ex/ey/ec hold the corners built in float (glint_corners) */
    int       hidden;     /* 1 = a captured cross line whose record the feed draws as a star instead */
    float     ex[4], ey[4];
    uint32_t  ec[4];      /* the corner colours, low 24 bits */
    double    bre, bim;   /* the outline basis at capture (glint_corners) */
    double    csx, csy;   /* the record point's projection at capture */
    glint_rec src;
} glint_item;

/* The screen-space rebase of one captured primitive onto the sub-tick pose:
 * the translation of the record's projected point and the rotation of the
 * corner offsets about it. Inactive on a canonical feed, and zero-valued at
 * the end of the tick's own interval, where the feed must reproduce the
 * canonical primitive bit for bit. */
typedef struct {
    float   dx, dy;   /* the record point's screen displacement */
    double  ang;      /* the spin phase this present shows, in sine-table entries */
    float   depth;    /* the depth of the re-projected record point */
    uint8_t active;   /* 1 = take `depth` and the shown corners or displacement */
    uint8_t corners;  /* 1 = cx/cy are the corners glint_corners built at the shown state */
    float   cx[4], cy[4];
    double  bre, bim; /* the outline basis at the shown state */
    double  csx, csy; /* the record point's projection at the shown state */
} glint_rebase;

static glint_item   s_item_buf[2][GLINT_MAX_PRIMS];
static glint_item*  s_item = s_item_buf[0];
static glint_rebase s_reb[GLINT_MAX_PRIMS];
static int          s_item_n;
/* The set the last capture of a new canonical epoch retired, which a held
 * camera cut shows when it is tick N-1's. */
static glint_item*  s_item_old = s_item_buf[1];
static int          s_item_old_n;
static int          s_cap_n;      /* items the call's capture walked; synthetic ones follow */
static int          s_stars;      /* draw-mode packets the call appended */
static int          s_crosses;    /* line pairs the call appended */
static int          s_pred_near;  /* near records the pool walk predicted */
static int          s_pred_far;   /* far records the pool walk predicted */
static int          s_hq_far;     /* far records the feed drew as stars this call (glint_star_far) */
static uint32_t     s_code_hist[256];
static uint32_t     s_tag_hist[256];
static uint32_t     s_fed_star_tris;
static uint32_t     s_fed_cross_tris;

/** @brief Exchange the current captured set with the retired one, with their counts. */
static void glint_sets_swap(void) {
    glint_item* t = s_item; s_item = s_item_old; s_item_old = t;
    const int n = s_item_n; s_item_n = s_item_old_n; s_item_old_n = n;
}

static cut_held_sets_t s_sets = { glint_sets_swap, &s_item_n, "glint", { 0u, 0u }, 0u };

/** @brief Append one walked primitive to s_item, unpaired at the near-guard depth.
 *
 * Also zeroes the item's record and its rebase slot.
 * @param ctx Unused.
 * @param s The decoded primitive.
 * @return 1 when stored, 0 when s_item holds GLINT_MAX_PRIMS. */
static int glint_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= GLINT_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 1.0f;   /* unpaired: the near guard (view-Z 1) */
    s_item[s_item_n].paired = 0;
    s_item[s_item_n].blend = 0;
    s_item[s_item_n].sub = 0;
    s_item[s_item_n].exact = 0;
    s_item[s_item_n].hidden = 0;
    memset(&s_item[s_item_n].src, 0, sizeof s_item[0].src);
    memset(&s_reb[s_item_n], 0, sizeof s_reb[0]);
    s_item_n++;
    return 1;
}

/** @brief Walk the polybuf interval the glint routine allocated into s_item.
 *
 * The interval holds 3-word draw-mode packets (tag length 2), 9-word quads
 * (tag length 8) and 4-word lines (tag length 3); a longer tag ends the walk.
 * At most GLINT_MAX_PRIMS prims are kept, each unpaired at the near-guard
 * depth. The code and tag histograms accumulate across calls.
 * @param cpu The guest CPU state, read only.
 * @param start First polybuf address of the routine's output.
 * @param end One past the last. */
static void glint_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 8, .raw_words_max = 9, .guard = GLINT_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, glint_prim_sink, NULL);
}

/** @brief Compute a record's camera-relative offset, halved the way the glint routine halves it.
 *
 * The routine halves with an arithmetic shift, which floors rather than
 * truncating toward zero (:2252-2258); every view-space quantity it computes
 * from the offset is therefore in half-world units. The components are ordered
 * (X - camX, camY - Y, camZ - Z), the order ngte_project_offset takes.
 * @param v The projection view; its camera position is read.
 * @param p The record's world position.
 * @param floored 1 floors each half as the routine does, for its verdicts; 0
 *        keeps the fraction, for placement.
 * @param vo Receives the halved offset. */
static void glint_view_offset(const ngte_view* v, const double p[3], int floored, double vo[3]) {
    vo[0] = (p[0] - v->cam[0]) / 2.0;      /* the routine's IR3 */
    vo[1] = (v->cam[1] - p[1]) / 2.0;      /* IR1 */
    vo[2] = (v->cam[2] - p[2]) / 2.0;      /* IR2 */
    if (floored) for (int k = 0; k < 3; k++) vo[k] = floor(vo[k]);
}

/** @brief Project a halved record offset the way the glint routine projects the star's origin.
 *
 * Rotates the offset by the camera matrix with a zero translation and
 * divides, which is the RTPS the routine runs on the star's own origin: its
 * outline points are transformed about this point, so it is where the star's
 * shared centre corner and the cross's midpoint land. The SXY bound is
 * dropped, as the particle feeder drops it, because widescreen reveal needs
 * the true X.
 * @param v The projection view.
 * @param vo The halved offset from glint_view_offset.
 * @param o Receives the screen point and the half-world view-space MACs. */
static void glint_project(const ngte_view* v, const double vo[3], ngte_proj* o) {
    ngte_project_offset(v, vo, o);
}

/** @brief Rotate a floored halved record offset by the camera matrix in the routine's integer arithmetic.
 *
 * The routine loads the halved offset as IR1 = camY - Y, IR2 = camZ - Z and
 * IR3 = X - camX and runs MVMVA with sf=1 against the rotation matrix
 * (:2252-2279), so each MAC is the dot product shifted right 12, an
 * arithmetic shift that floors. Its depth window, its pyramid tests
 * 4|MAC1| - 3 MAC3 and 3|MAC2| - MAC3, and its near/far switch all read these
 * MACs (:2294-2348). A fractional MAC agrees with the floored one on every
 * test of MAC3 alone against an integer bound, but not on the pyramid tests:
 * a record within a few units of a pyramid bound takes a different verdict,
 * the predicted counts then disagree with the captured ones, and the
 * pairing leaves the whole call unowned.
 * @param v The projection view; its matrix is read.
 * @param vo The floored halved offset from glint_view_offset.
 * @param mac Receives MAC1, MAC2 and MAC3. */
static void glint_mac_int(const ngte_view* v, const double vo[3], int64_t mac[3]) {
    const int64_t ir[3] = { (int64_t)vo[1], (int64_t)vo[2], (int64_t)vo[0] };
    for (int r = 0; r < 3; r++) {
        const int64_t d = (int64_t)v->cam_M[r][0] * ir[0] + (int64_t)v->cam_M[r][1] * ir[1] +
                          (int64_t)v->cam_M[r][2] * ir[2];
        mac[r] = d >= 0 ? d >> 12 : -((-d + 4095) >> 12);   /* floor(d / 4096) */
    }
}

/* ---- owner ----------------------------------------------------------------
 *
 * The candidates are the mobys the canonical pass drew this tick: the
 * animated models' cull records (moby_native_canon_drawn_mobys), the host
 * reveal's admissions (moby_native_revealed_mobys) and the simple models'
 * queue (moby_native_simple_mobys), all three filled by the moby draw the
 * world draw runs before the glint routine (Spyro 2 drawutil/func_8004C534.s:17-21
 * from draw/func_800155A0.s:16, the glint routine at :32; Spyro 3
 * drawutil/func_8001EC5C.s:15-19 from draw/func_8001E460.s:14, the glint
 * routine at :30). */

#define GLINT_OWN_MAX   512    /* candidates kept per call */
#define GLINT_RAD_CACHE 64     /* simple-model headers whose radius is kept */
#define GLINT_RAD_VERTS 256    /* the most vertices one simple-model list holds */

typedef struct {
    uint32_t moby;
    double   pos[3];   /* its world origin */
    float    rad;      /* its model radius, world units */
} glint_owner;

static glint_owner s_own[GLINT_OWN_MAX];
static int         s_own_n;
static double      s_own_reach;   /* the farthest a record sits from its owner's origin */

/* One simple-model header's radius, keyed by the header address and the words
 * a different model at the same address would change: the vertex counts, the
 * bounding bytes and the first and last vertex. */
typedef struct {
    uint32_t hdr, h0, h1, v0, vn;
    float    rad;
} glint_rad_ent;

static glint_rad_ent s_rad[GLINT_RAD_CACHE];
static unsigned      s_rad_next;

/** @brief The farthest a glint record sits from the origin of the moby that placed it, in world units.
 *
 * A gem handler places its near record at the gem's origin plus the gem's
 * rotation times one point of the sparkle table (Spyro 3
 * overlays/level_28/moby/func_level_28_80074A0C.s:152-168, Spyro 2
 * overlays/level_50/moby_update/func_level_50_800705C8.s:433-444), its far
 * record at the origin plus 0x80 Z (func_level_28_80074A0C.s:97-111), and an
 * orb at its origin. The rotation is a Q12 matrix of unit length, so the
 * reach is the longest table point plus 2 for the rotation's truncation to
 * integers on each axis. A layout that locates no table gives the far
 * record's 0x80.
 * @param cpu Reads the sparkle table.
 * @return The reach. */
static double glint_sparkle_reach(CPUState* cpu) {
    double far2 = 128.0 * 128.0;
    for (uint32_t i = 0; GT.sparkle && i < GT.sparkle_n; i++) {
        double d2 = 0.0;
        for (uint32_t k = 0; k < 3u; k++) {
            const double c = (double)(int32_t)cpu->read_word(GT.sparkle + i * 12u + k * 4u);
            d2 += c * c;
        }
        if (d2 > far2) far2 = d2;
    }
    return sqrt(far2) + 2.0;
}

/** @brief The largest distance of a simple model's vertices from its origin, in world units.
 *
 * The draw reads the vertex words at header + 0x10, a high-poly count in
 * byte 1 of the header's first word and a low-poly count in byte 2 over the
 * same list, and unpacks each word into X in bits 11-21, Y in bits 0-11 and Z
 * in bits 21-31 in half-world units (moby_game_spyro23.c, s23_simple_pass).
 * The result is cached per header (GLINT_RAD_CACHE entries, replaced in
 * turn).
 * @param cpu Reads the header and its vertices.
 * @param hdr The geometry header, KSEG0.
 * @return The radius, 0 for a model with no vertex. */
static float glint_simple_radius(CPUState* cpu, uint32_t hdr) {
    const uint32_t h0 = cpu->read_word(hdr), h1 = cpu->read_word(hdr + 4u);
    uint32_t n = (h0 >> 8) & 0xFFu;
    if (((h0 >> 16) & 0xFFu) > n) n = (h0 >> 16) & 0xFFu;
    if (n > GLINT_RAD_VERTS) n = GLINT_RAD_VERTS;
    if (n == 0u) return 0.0f;
    const uint32_t v0 = cpu->read_word(hdr + 0x10u), vn = cpu->read_word(hdr + 0x10u + (n - 1u) * 4u);
    for (int i = 0; i < GLINT_RAD_CACHE; i++) {
        const glint_rad_ent* e = &s_rad[i];
        if (e->hdr == hdr && e->h0 == h0 && e->h1 == h1 && e->v0 == v0 && e->vn == vn) return e->rad;
    }
    double far2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t w = cpu->read_word(hdr + 0x10u + i * 4u);
        const double x = (double)((int32_t)(w << 10) >> 21), y = (double)((int32_t)(w << 20) >> 20),
                     z = (double)((int32_t)w >> 21);
        const double d2 = x * x + y * y + z * z;
        if (d2 > far2) far2 = d2;
    }
    glint_rad_ent* e = &s_rad[s_rad_next++ % GLINT_RAD_CACHE];
    *e = (glint_rad_ent){ hdr, h0, h1, v0, vn, (float)(2.0 * sqrt(far2)) };
    return e->rad;
}

/** @brief A moby's model radius, the largest distance from its origin of a vertex the native draw emits for it, in world units.
 *
 * A simple model takes glint_simple_radius of the geometry header at
 * slot + 0x14, the slot being the model plus four times the animation byte
 * (Spyro 3 mobydraw.s:109-117, queued at :628). An animated model takes the
 * animation header's radius byte (byte 7) times 64: the cull's sphere
 * radius, the byte times 16 in quarter-world units.
 * @param cpu The guest CPU state, read only.
 * @param m The Moby address.
 * @return The radius, or 0 when the moby has no readable model. */
static float glint_model_radius(CPUState* cpu, uint32_t m) {
    const uint32_t cls = cpu->read_half(m + game_layout.moby.class_off);
    const uint32_t model = cpu->read_word(game_layout.moby.model_table + cls * 4u);
    const uint32_t anim = cpu->read_byte(m + game_layout.moby.anim_state_off);
    if (model == 0u || (model & 0x7FFFFFFFu) >= 0x00800000u) return 0.0f;
    if (moby_game.model_is_animated(cpu, model)) {
        const uint32_t hdr = cpu->read_word(model + game_layout.moby.model_anims_off + anim * 4u);
        if (hdr < 0x80000000u || hdr >= 0x80800000u) return 0.0f;
        return (float)((uint32_t)cpu->read_byte(hdr + game_layout.moby.anim_radius) << 6);
    }
    const uint32_t hdr = cpu->read_word(model + anim * 4u + 0x14u);
    if (hdr < 0x80000000u || hdr >= 0x80800000u) return 0.0f;
    return glint_simple_radius(cpu, hdr);
}

/** @brief Add each moby of one drawn set that has a model radius to the owner candidates.
 * @param cpu The guest CPU state, read only.
 * @param set The mobys.
 * @param n The set's length. */
static void glint_owner_add(CPUState* cpu, const uint32_t* set, int n) {
    for (int i = 0; i < n && s_own_n < GLINT_OWN_MAX; i++) {
        const uint32_t m = set[i];
        if (m < 0x80000000u || m >= 0x80800000u) continue;
        const float rad = glint_model_radius(cpu, m);
        if (rad <= 0.0f) continue;
        glint_owner* o = &s_own[s_own_n++];
        o->moby = m;
        o->rad = rad;
        for (uint32_t k = 0; k < 3u; k++)
            o->pos[k] = (double)(int32_t)cpu->read_word(m + game_layout.moby.pos_off + k * 4u);
    }
}

/** @brief Whether records take owners, latched on first use.
 *
 * OPENPETE_GLINT_OWN=0 names no owner, so every star keeps its record's
 * depth: the old-depth leg of the owner A/B. Any other value or none enables
 * the owners.
 * @return 1 when enabled, else 0. */
static int glint_own_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLINT_OWN"); v = !(e && *e == '0'); }
    return v;
}

/** @brief Collect this tick's owner candidates and the sparkle reach.
 *
 * Collects none when glint_own_enabled says no.
 * @param cpu The guest CPU state, read only. */
static void glint_owner_scan(CPUState* cpu) OP_REQUIRES(sim) {
    static uint32_t simple[GLINT_OWN_MAX];
    s_own_n = 0;
    s_own_reach = glint_sparkle_reach(cpu);
    if (!glint_own_enabled()) return;
    const uint32_t* set;
    int n = moby_native_canon_drawn_mobys(&set);
    glint_owner_add(cpu, set, n);
    n = moby_native_revealed_mobys(&set);
    glint_owner_add(cpu, set, n);
    n = moby_native_simple_mobys(simple, GLINT_OWN_MAX);
    glint_owner_add(cpu, simple, n);
}

/** @brief Name a record's owner: the nearest candidate whose origin lies within the sparkle reach of the record.
 *
 * Fills the record's owner vector and radius; a record with no candidate in
 * reach gets radius 0.
 * @param p The record's world position.
 * @param out The record. */
static void glint_owner_pick(const double p[3], glint_rec* out) {
    double best = s_own_reach * s_own_reach;
    int pick = -1;
    for (int i = 0; i < s_own_n; i++) {
        const double dx = s_own[i].pos[0] - p[0], dy = s_own[i].pos[1] - p[1],
                     dz = s_own[i].pos[2] - p[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= best) { best = d2; pick = i; }
    }
    out->own_rad = pick >= 0 ? s_own[pick].rad : 0.0f;
    for (int k = 0; k < 3; k++) out->own[k] = pick >= 0 ? s_own[pick].pos[k] - p[k] : 0.0;
}

/** @brief The half-world view-Z a star is drawn at: its record's, or its owner's sphere front where that is nearer.
 *
 * The owner's origin is the record's position plus the record's owner
 * vector, projected under the same view; the sphere front is its view-Z less
 * half the owner's radius (the view-Z is half-world). A front behind the
 * near guard of 1 half-world unit is held there, so the pull never puts a
 * star behind the camera plane. A record with no owner keeps its own view-Z.
 * @param v The view.
 * @param pos The record's world position this present shows.
 * @param rec_vz The half-world view-Z of that position.
 * @param r The record; its owner vector and radius are read.
 * @return The view-Z, never farther than rec_vz. */
static double glint_star_vz(const ngte_view* v, const double pos[3], double rec_vz,
                            const glint_rec* r) {
    if (r->own_rad <= 0.0f) return rec_vz;
    const double op[3] = { pos[0] + r->own[0], pos[1] + r->own[1], pos[2] + r->own[2] };
    double vo[3];
    ngte_proj o;
    glint_view_offset(v, op, 0, vo);
    glint_project(v, vo, &o);
    const double front = fmax(o.m3 - (double)r->own_rad / 2.0, 1.0);
    return front < rec_vz ? front : rec_vz;
}

/** @brief Mirror the glint routine's rejects for one pool record and, on acceptance, fill in what the pairing and the sub-tick rebase read from it.
 *
 * Rejects a record whose lifetime byte is 0 (:2236-2240), whose halved
 * camera-relative offset leaves the +/-GT.box box (:2266-2276), whose
 * half-world view Z is below GT.near_z or at least GT.far_z (:2294-2296), or
 * that leaves the view pyramid 4|X| - 3Z < GT.pyr_x, 3|Y| - Z < GT.pyr_y
 * (:2310, :2320), each test on the integer MACs the routine tests
 * (glint_mac_int). The box
 * and far-depth rejects and the lateral reject are reported as flags rather
 * than verdicts, with every other test still applied, so the extended-LOD and
 * widened-view reveals can draw such a record. An accepted record takes the
 * near branch when its view Z is below GT.near_vz; its projected point and
 * view Z come from the unfloored offset, its owner from glint_owner_pick, and
 * its scene depth from glint_star_vz scaled back to world units.
 * @param cpu The guest CPU state, read only.
 * @param v The projection view.
 * @param rec Guest address of the record.
 * @param out Receives the record's branch, depth, address, position words,
 *        lifetime, size, spin and colour bytes, projected point, view Z,
 *        owner and pull whenever the return is nonzero; untouched otherwise.
 * @return 1 when the routine would draw the record; else GLINT_RANGE when it
 *         rejects the record on the box or the far depth, or-ed with
 *         GLINT_LATERAL when it fails the lateral test 4|X| - 3Z < GT.pyr_x,
 *         when those are its only rejects; else 0. */
static int glint_accept(CPUState* cpu, const ngte_view* v, uint32_t rec,
                        glint_rec* out) {
    const uint32_t packed = cpu->read_word(rec + GT.packed_off);
    if ((packed & 0xFFu) == 0u) return 0;          /* :2236-2240 expired or free */

    uint32_t w[3];
    double p[3], vo[3];
    for (int k = 0; k < 3; k++) {
        w[k] = cpu->read_word(rec + GT.pos_off + (uint32_t)k * 4u);
        p[k] = (double)(int32_t)w[k];
    }
    glint_view_offset(v, p, 1, vo);
    const double box = (double)GT.box;
    int range = 0;
    for (int k = 0; k < 3; k++)
        if (vo[k] >= box || vo[k] <= -box) range = 1;            /* :2266-2276 */

    int64_t mac[3];
    glint_mac_int(v, vo, mac);
    if (mac[2] < GT.near_z) return 0;                                   /* :2294 */
    if (mac[2] >= GT.far_z) range = 1;                                  /* :2296 */
    const int lateral = 4 * llabs(mac[0]) - 3 * mac[2] - GT.pyr_x >= 0;   /* :2310 */
    if (3 * llabs(mac[1]) - mac[2] - GT.pyr_y >= 0) return 0;             /* :2320 */

    out->near_mode = (mac[2] < GT.near_vz);
    /* The verdicts above read the routine's integer MACs; the placement
     * reads the unfloored offset. */
    ngte_proj o;
    glint_view_offset(v, p, 0, vo);
    glint_project(v, vo, &o);
    glint_owner_pick(p, out);
    const double svz = glint_star_vz(v, p, o.m3, out);
    out->pull = (float)(2.0 * (o.m3 - svz));
    out->depth = openpete_native_scene_viewz(
        openpete_worldvz_scaled((float)svz, 2.0f));
    out->addr = rec;
    for (int k = 0; k < 3; k++) out->w[k] = w[k];
    out->life = (uint8_t)(packed & 0xFFu);
    out->size = (uint8_t)((packed >> 8) & 0xFFu);
    out->spin = (uint8_t)((packed >> 16) & 0xFFu);
    out->row  = (uint8_t)(packed >> 24);
    out->psx = o.sx;
    out->psy = o.sy;
    out->pvz = o.m3;
    ngte_view canon;
    ngte_view_canon(v, &canon);
    glint_project(&canon, vo, &o);
    out->gsx = o.sx;
    out->gsy = o.sy;
    const int flags = (range ? GLINT_RANGE : 0) | (lateral ? GLINT_LATERAL : 0);
    return flags ? flags : 1;
}

/** @brief Re-walk the glint pool in slot order and collect every record the routine would have drawn.
 *
 * Each of the GT.slots records is tested with glint_accept. Entries past `cap`
 * are counted but not stored.
 * @param cpu The guest CPU state, read only.
 * @param v The projection view.
 * @param out Receives up to `cap` accepted records in pool order.
 * @param cap Capacity of `out`.
 * @return The number of accepted records, which may exceed `cap`. */
static int glint_predict(CPUState* cpu, const ngte_view* v, glint_rec* out, int cap) {
    int n = 0;
    for (uint32_t s = 0; s < GT.slots; s++) {
        glint_rec r;
        if (glint_accept(cpu, v, GT.pool + s * GT.stride, &r) != 1) continue;
        if (n < cap) out[n] = r;
        n++;
    }
    return n;
}

/** @brief Count the captured draw-mode packets and lines.
 *
 * One draw-mode packet opens a near star; each consecutive pair of lines is
 * one far cross.
 * @param stars Receives the number of draw-mode packets.
 * @param lines Receives the number of lines. */
static void glint_count_prims(int* stars, int* lines) {
    *stars = 0; *lines = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (s_item[i].p.code == GLINT_DRMODE) (*stars)++;
        else if (s_item[i].p.code == GLINT_LINE) (*lines)++;
    }
}

/** @brief Leave every captured primitive unowned at the near-guard depth rather than dropping it.
 *
 * Sets each primitive's blend from its semi-transparency bit (1 when set, else
 * opaque) and clears its record address. An unowned primitive still draws,
 * holds its tick pose across the sub-tick presents for want of a record to
 * re-project, and counts as unpaired in the census. */
static void glint_unpair_all(void) {
    for (int i = 0; i < s_item_n; i++) {
        s_item[i].blend = s_item[i].p.stp ? 1 : 0;
        s_item[i].src.addr = 0u;
    }
}

/** @brief Advance the prediction cursor to the next record of one branch.
 *
 * The pool order the routine drew in is the order the predictions came out
 * in, and the two branches interleave in that order, so one cursor over the
 * predictions serves both.
 * @param pred The predicted records in pool order.
 * @param predicted Their count.
 * @param r The current cursor, -1 before the first record.
 * @param near_mode 1 to find the next near record, 0 the next far one.
 * @return The index of that record, or `predicted` when the branch has none
 *         left. */
static int glint_next_rec(const glint_rec* pred, int predicted, int r, int near_mode) {
    while (++r < predicted && pred[r].near_mode != near_mode) { }
    return r;
}

/** @brief Attach one captured primitive to the record that emitted it.
 *
 * Copies the record into the item, takes the record's depth, sets the blend
 * and marks the item paired.
 * @param it The captured primitive.
 * @param r The record the pairing cursor stands on.
 * @param blend The native blend mode for the primitive. */
static void glint_bind(glint_item* it, const glint_rec* r, int blend) {
    it->blend = blend;
    it->depth = r->depth;
    it->src = *r;
    it->paired = 1;
}

/** @brief Walk the captured primitives and the predicted records together and bind each primitive to its record.
 *
 * A draw-mode packet advances to the next near record and its ABR bits give
 * the star's blend; the quads until the next packet belong to that record.
 * The first line of each consecutive pair advances to the next far record,
 * opaque. A quad before any draw-mode packet stays unpaired, and the walk
 * stops when a branch runs out of records. The draw-mode packets themselves
 * are not bound.
 * @param pred The predicted records in pool order.
 * @param predicted Their count. */
static void glint_pair_walk(const glint_rec* pred, int predicted) {
    const glint_rec* cur = NULL;
    int r = -1, blend = 0, line_in_cross = 0, quad = 0;
    for (int i = 0; i < s_item_n; i++) {
        const uint8_t code = s_item[i].p.code;
        if (code == GLINT_DRMODE) {
            r = glint_next_rec(pred, predicted, r, 1);
            if (r >= predicted) return;
            /* The draw-mode word's semi-transparency rate, bits 5 and 6. */
            blend = ngte_abr_blend((int)((s_item[i].p.raw[1] >> 5) & 3u),
                                   NGTE_ABR_QUARTER_AS_ADD);
            cur = &pred[r];
            quad = 0;
            continue;
        }
        if (code == GLINT_LINE) {
            if (line_in_cross == 0) {
                r = glint_next_rec(pred, predicted, r, 0);
                if (r >= predicted) return;
                blend = 0;                      /* :2480 the far cross is opaque */
                cur = &pred[r];
            }
            s_item[i].sub = line_in_cross;
            line_in_cross = (line_in_cross + 1) % GLINT_LINES_PER_CROSS;
        } else if (!cur) {
            continue;                           /* a quad before any draw mode */
        } else {
            s_item[i].sub = quad++;
        }
        glint_bind(&s_item[i], cur, blend);
    }
}

static unsigned long s_glint_order, s_glint_nearest, s_glint_depth1;

/** @brief The end of the primitive group starting at a draw-mode packet or a line, and its vertex centroid.
 *
 * A star group is its draw-mode packet and the quads that follow it; a cross
 * group is up to GLINT_LINES_PER_CROSS consecutive lines. The centroid
 * averages the group's quad or line vertices.
 * @param i The group's first item.
 * @param cx Receives the centroid's X.
 * @param cy Receives its Y.
 * @return One past the group's last item. */
static int glint_group_end(int i, double* cx, double* cy) {
    const int star = s_item[i].p.code == GLINT_DRMODE;
    int e = i + 1, n = 0;
    if (!star) e = i;
    double sx = 0.0, sy = 0.0;
    while (e < s_item_n && (star ? s_item[e].p.code == GLINT_QUAD
                                 : (s_item[e].p.code == GLINT_LINE && e - i < GLINT_LINES_PER_CROSS))) {
        const int nv = s_item[e].p.code == GLINT_QUAD ? 4 : 2;
        for (int k = 0; k < nv; k++) { sx += s_item[e].p.x[k]; sy += s_item[e].p.y[k]; n++; }
        e++;
    }
    *cx = n ? sx / n : 0.0;
    *cy = n ? sy / n : 0.0;
    return e > i ? e : i + 1;
}

/** @brief The predicted record nearest a screen point, of the given branch when one exists, else of either.
 * @param pred The predicted records.
 * @param predicted Their count.
 * @param cx The point's X, in the routine's screen space.
 * @param cy Its Y.
 * @param near_mode The branch to prefer: 1 near star, 0 far cross.
 * @return The record's index, or -1 when there is none. */
static int glint_nearest_rec(const glint_rec* pred, int predicted, double cx, double cy, int near_mode) {
    int best = -1, best_mode = 0;
    double bd = INFINITY;
    for (int r = 0; r < predicted; r++) {
        const int mode = pred[r].near_mode == near_mode;
        const double d = (pred[r].gsx - cx) * (pred[r].gsx - cx) + (pred[r].gsy - cy) * (pred[r].gsy - cy);
        if (mode > best_mode || (mode == best_mode && d < bd)) { best = r; bd = d; best_mode = mode; }
    }
    return best;
}

/** @brief Binds each star and cross group of a call whose predicted counts disagree with the captured ones to its nearest predicted record.
 *
 * The record is the one whose point under the GTE's own H lies nearest the
 * group's centroid, of the group's branch when one was predicted
 * (glint_nearest_rec). A star takes the blend of its draw-mode packet, a
 * cross is opaque. A group with no predicted record stays unowned at the
 * near-guard depth. Counts the bound groups and the unowned ones.
 * @param pred The predicted records in pool order.
 * @param predicted Their count. */
static void glint_pair_nearest(const glint_rec* pred, int predicted) {
    int i = 0;
    while (i < s_item_n) {
        const uint8_t code = s_item[i].p.code;
        if (code != GLINT_DRMODE && code != GLINT_LINE) { i++; continue; }
        double cx, cy;
        const int e = glint_group_end(i, &cx, &cy);
        const int r = glint_nearest_rec(pred, predicted, cx, cy, code == GLINT_DRMODE);
        if (r < 0) { s_glint_depth1++; i = e; continue; }
        const int blend = code == GLINT_DRMODE
            ? ngte_abr_blend((int)((s_item[i].p.raw[1] >> 5) & 3u), NGTE_ABR_QUARTER_AS_ADD) : 0;
        for (int k = (code == GLINT_DRMODE) ? i + 1 : i, sub = 0; k < e; k++, sub++) {
            s_item[k].sub = sub;
            glint_bind(&s_item[k], &pred[r], blend);
        }
        s_glint_nearest++;
        i = e;
    }
}

/** @brief Pair the call's captured primitives with the pool records that emitted them.
 *
 * Counts the captured stars and crosses into s_stars and s_crosses, predicts
 * the accepted records into s_pred_near and s_pred_far, and runs
 * glint_pair_walk only when the predicted near and far counts both equal the
 * captured ones, the line count is even and the prediction fits the table;
 * otherwise every primitive is first left unowned by glint_unpair_all and
 * then bound group by group to its nearest predicted record
 * (glint_pair_nearest).
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view. */
static void glint_pair_depths(CPUState* cpu, const ngte_view* v) {
    static glint_rec pred[GLINT_MAX_PRIMS];
    int lines = 0;
    glint_count_prims(&s_stars, &lines);
    s_crosses = lines / GLINT_LINES_PER_CROSS;

    const int predicted = glint_predict(cpu, v, pred, GLINT_MAX_PRIMS);
    s_pred_near = 0; s_pred_far = 0;
    for (int i = 0; i < predicted && i < GLINT_MAX_PRIMS; i++) {
        if (pred[i].near_mode) s_pred_near++; else s_pred_far++;
    }
    if (predicted > GLINT_MAX_PRIMS || s_pred_near != s_stars ||
        s_pred_far != s_crosses || lines % GLINT_LINES_PER_CROSS != 0) {
        glint_unpair_all();
        glint_pair_nearest(pred, predicted < GLINT_MAX_PRIMS ? predicted : GLINT_MAX_PRIMS);
        return;
    }
    s_glint_order += (unsigned long)(s_stars + s_crosses);
    glint_pair_walk(pred, predicted);
}

/** @brief Read OPENPETE_GLINT, which selects what this feeder emits, latched on first use.
 *
 * 0 feeds nothing, 1 the near stars alone, 2 or more the near stars and the
 * far crosses (the default when unset or empty); a negative value reads as 0.
 * The guest draw runs at every setting, so the A/B prices the painted pixels
 * from one binary.
 *
 * The far cross is fed at its own projected depth, with no ordering-table pull
 * applied: the routine keys it on the same view-Z the depth comes from, so the
 * depth test reproduces the ordering the routine asked for. Where world
 * geometry stands between the camera and the gem, the cross is covered - in
 * the ordering table and under the depth test alike - and where nothing does,
 * it draws.
 * @return The mode, at least 0. */
static int glint_feed_mode(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_GLINT");
        v = (e && *e) ? atoi(e) : 2;
        if (v < 0) v = 0;
    }
    return v;
}

/* ---- sub-tick rebase ------------------------------------------------------
 *
 * The primitives are built under the tick's camera from the tick's pool, so a
 * sub-tick present that re-fed them would draw every star at the pixels the
 * tick drew: neither the camera's motion nor the record's own would reach
 * them, and neither would the spin and the size. The rebase blends the
 * record's position, lifetime and spin phase toward the previous tick's and
 * rebuilds the corners from them under the staged camera (glint_corners). At
 * the end of the tick's own interval the blended state and the staged camera
 * are the tick's, so the rebuild takes identical inputs through identical
 * arithmetic and reproduces the canonical corners exactly.
 *
 * The routine builds the outline in screen-aligned view space: it loads a 2x2
 * rotation-scale basis into the GTE rotation registers and transforms the
 * outline about the record's own translation (:2324-2330 for the star,
 * :2480-2487 for the cross), so all of a star's corners share one view Z and
 * the perspective divide is common to them. */

/* A gem's speed ceiling, in world units per tick. The table is keyed by slot
 * address, so a slot a new record respawns into inside one tick pairs across
 * two different gems and would lerp as a streak between them. The packed-word
 * test below catches a reuse that changed the size byte, the spin phase or the
 * colour row, and the lifetime test one that kept all three; this catches the
 * rest, at a distance no gem covers in a tick. */
#define GLINT_REUSE_CEILING 4096.0

/** @brief The spin phase in sine-table entries, 256 to the turn, of a record at a given remaining lifetime.
 *
 * The phase is the phase byte less four entries per lifetime step (:2270-2277
 * masks the difference to a byte and doubles it for the halfword index), so a
 * record turns four entries per lifetime step as its lifetime counts down. A
 * fractional lifetime gives the phase between two steps; an integral one
 * gives the routine's own byte.
 * @param spin The record's spin-phase byte.
 * @param life The remaining lifetime, fractional on a sub-tick present.
 * @return The phase in [0, 256). */
static double glint_phase_at(uint8_t spin, double life) {
    const double a = fmod((double)spin - 4.0 * life, 256.0);
    return a < 0.0 ? a + 256.0 : a;
}

/** @brief The spin phase the routine drew a record at, in sine-table entries.
 *
 * @param r The record as the tick read it.
 * @return glint_phase_at of the record's phase byte and lifetime byte. */
static double glint_angle(const glint_rec* r) {
    return glint_phase_at(r->spin, (double)r->life);
}

/** @brief Compute a record's displacement, phase and lifetime between the pose the tick drew and the pose this present shows.
 *
 * Reads the record's previous snapshot from the interp object table and
 * blends toward it by 1 - alpha. Returns 0, leaving the tick's position,
 * phase and lifetime, for a record with no previous snapshot, a snapshot
 * blob too short for the position and packed words, a pair whose prev was
 * not observed up to curr (cut_feeder_pair_is_tick), a reused slot, or a displacement beyond
 * GLINT_REUSE_CEILING; the primitive then moves only with the camera.
 *
 * Every byte of the packed word but the lifetime is written once by the
 * spawner and never again, so an unequal upper three bytes is a slot that took
 * a new record; the lifetime itself only ever falls while a record lives, so a
 * risen lifetime is a reuse that happened to keep them.
 * @param r The paired record as the tick read it.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @param off Receives the world-unit offset from the tick's position.
 * @param show_ang Receives the spin phase the present shows.
 * @param show_life Receives the lifetime the present shows.
 * @return 1 when the previous snapshot is the same gem, else 0. */
static int glint_record_pair(const glint_rec* r, double alpha, double off[3],
                             double* show_ang, double* show_life) {
    off[0] = off[1] = off[2] = 0.0;
    *show_ang = glint_angle(r);
    *show_life = (double)r->life;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(r->addr);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    if ((uint32_t)o->blob_len < GT.pos_off + 12u ||
        (uint32_t)o->blob_len < GT.packed_off + 4u) return 0;
    if (!cut_feeder_pair_is_tick("glint", r->addr, o, GT.pos_off, 12u)) return 0;
    uint32_t pp, cp;
    memcpy(&pp, o->prev + GT.packed_off, 4);
    memcpy(&cp, o->curr + GT.packed_off, 4);
    if ((pp & ~0xFFu) != (cp & ~0xFFu)) return 0;        /* the slot took a new record */
    if ((cp & 0xFFu) > (pp & 0xFFu)) return 0;           /* the lifetime rose: a reuse */
    int32_t pw[3];
    memcpy(pw, o->prev + GT.pos_off, 12);
    double d[3], sq = 0.0;
    for (int k = 0; k < 3; k++) {
        d[k] = (double)pw[k] - (double)(int32_t)r->w[k];
        sq += d[k] * d[k];
    }
    if (sq > GLINT_REUSE_CEILING * GLINT_REUSE_CEILING) return 0;
    const double rest = 1.0 - alpha;
    for (int k = 0; k < 3; k++) off[k] = d[k] * rest;
    /* The lifetime falls monotonically while the record lives (the reuse tests
     * above), so the shown lifetime is the straight blend of the two bytes.
     * The phase byte is the same in both (the packed-word test above), so the
     * shown phase is the routine's phase at the shown lifetime. */
    *show_life = (double)r->life + ((double)(pp & 0xFFu) - (double)r->life) * rest;
    *show_ang = glint_phase_at(r->spin, *show_life);
    return 1;
}

/** @brief The lifetime factor of the star's size, a triangular ramp on the remaining lifetime.
 *
 * L below 0x20 and 2 * (0x30 - L) from there, plus eight
 * (mobyfunc.s:2278-2285, Spyro 2 8004ADC4-8004ADDC; Spyro 3 8002F1E4-8002F1FC).
 * The routine multiplies it by the size byte (:2286, 8004ADE0 / 8002F200) and
 * scales the sine pair of the rotation basis by that product (:2324 and :2333,
 * 8004AE70 and 8004AE94 / 8002F290 and 8002F2B4), so the star's outline and
 * the cross's arms are proportional to it. Evaluated at a fractional lifetime
 * it is the same two line segments, continuous at 0x20.
 * @param life The remaining lifetime L.
 * @return The ramp value. */
static double glint_size_ramp(double life) {
    return (life < 32.0 ? life : 2.0 * (48.0 - life)) + 8.0;
}

/* The far cross's four arm ends follow the star's seventeen points in the
 * outline table (Spyro 2 mobyfunc.s:2454, Spyro 3 :2457). */
#define GLINT_CROSS_OFF 0x44u

/* The state one star or cross is built at: the record's world position, spin
 * phase and remaining lifetime, each fractional on a sub-tick present, and
 * the half-world view-Z whose screen extent a held extent keeps (0 = none). */
typedef struct {
    double pos[3];
    double ang;
    double life;
    double hold_vz;
} glint_pose;

/** @brief Build one primitive's corners in float from its record, the camera and the routine's tables.
 *
 * The routine halves the record's camera-relative offset with an arithmetic
 * shift and rotates it by the camera matrix with an integer MVMVA
 * (mobyfunc.s:2252-2258, :2279); the MACs become the GTE translation. It scales
 * the sine pair at the spin phase by the size ramp times the size byte, shifted
 * right 14 (:2324-2346), and loads the 2x2 basis rows (c, -s) and (s, c) into
 * the rotation registers (:2350-2358). The near star RTPSes the seventeen
 * outline points against it (:2366-2373) and each quad takes four of them by
 * the index word of the quad table (:2394-2405); the far cross first multiplies
 * both basis terms by the view-Z and shifts them right 13 (:2433-2444), then
 * RTPSes the four arm ends, line 0 joining ends 0 and 2 and line 1 ends 1 and 3
 * (:2476-2479). Every step here stays fractional: the halved offset, the
 * rotation, the sine pair interpolated at a fractional phase (ngte_sincos_f),
 * the basis without its shift, the MACs of each point and the perspective
 * divide (ngte_screen_gte with no SXY bound, so a widened view keeps the true
 * X). Corner colours come from the colour table row: a quad's corners 0 and 3
 * take the waist word, corner 1 the tip word and corner 2 (the shared centre)
 * the pale word (:2390-2393); a line takes the pale word (:2480-2482).
 * @param cpu Reads the outline, quad, colour and sine tables.
 * @param v The view the corners project under.
 * @param r The record; supplies the branch the size scale follows, the size
 *        byte and the colour row.
 * @param ps The state to build at.
 * @param star 1 builds a quad of the near branch's star, 0 a line of the far
 *        branch's cross, whichever branch the routine took for the record.
 * @param sub The quad (0-7) or line (0-1) of the record.
 * @param x Receives the screen X of each corner.
 * @param y Receives the screen Y of each corner.
 * @param rgb Receives each corner's colour, low 24 bits.
 * @param ctr Receives the record point's projection.
 * @param basis Receives the outline basis (c, s) over the view-Z, the factor
 *        a corner's screen offset from the record point is proportional to.
 * @return The corner count, 4 for a quad and 2 for a line, or 0 when the
 *         layout locates no outline, quad or colour table. */
static int glint_corners(CPUState* cpu, const ngte_view* v, const glint_rec* r,
                         const glint_pose* ps, int star, int sub, float x[4], float y[4],
                         uint32_t rgb[4], ngte_proj* ctr, double basis[2]) {
    if (!GT.outline || !GT.quads || !GT.colours) return 0;
    const double vo[3] = { (ps->pos[0] - v->cam[0]) / 2.0, (v->cam[1] - ps->pos[1]) / 2.0,
                           (v->cam[2] - ps->pos[2]) / 2.0 };
    glint_project(v, vo, ctr);
    double sn, cs;
    ngte_sincos_f(cpu, ps->ang, &sn, &cs);
    double k = glint_size_ramp(ps->life) * (double)r->size / 16384.0;
    /* The view-Z scale follows the record's branch, not the drawn kind: a far
     * record built as a star keeps the screen extent of the cross the routine
     * drew for it, equal to the near star's at the switch depth. */
    if (r->near_mode && ps->hold_vz > 0.0 && ctr->m3 > 0.0) k *= ctr->m3 / ps->hold_vz;
    if (!r->near_mode) k *= ctr->m3 / 8192.0;
    const double c = k * cs, sv = k * sn;
    const double z = ctr->m3 != 0.0 ? ctr->m3 : 1.0;
    basis[0] = c / z;
    basis[1] = sv / z;
    const uint32_t row = GT.colours + (uint32_t)r->row * 12u;
    uint32_t pt[4];
    int n;
    if (star) {
        const uint32_t w = cpu->read_word(GT.quads + (uint32_t)(sub & 7) * 4u);
        for (int j = 0; j < 4; j++) pt[j] = (w >> (24 - 8 * j)) & 0xFFu;
        const uint32_t pale = cpu->read_word(row), tip = cpu->read_word(row + 4u),
                       waist = cpu->read_word(row + 8u);
        rgb[0] = waist & 0xFFFFFFu; rgb[1] = tip & 0xFFFFFFu;
        rgb[2] = pale & 0xFFFFFFu;  rgb[3] = waist & 0xFFFFFFu;
        n = 4;
    } else {
        const uint32_t l = (uint32_t)(sub & 1);
        pt[0] = (GLINT_CROSS_OFF / 4u) + l;
        pt[1] = (GLINT_CROSS_OFF / 4u) + l + 2u;
        rgb[0] = rgb[1] = cpu->read_word(row) & 0xFFFFFFu;
        n = 2;
    }
    for (int j = 0; j < n; j++) {
        const uint32_t w = cpu->read_word(GT.outline + pt[j] * 4u);
        const double px = (double)(int16_t)(w & 0xFFFFu), py = (double)(int16_t)(w >> 16);
        ngte_proj o;
        ngte_screen_gte(ctr->m1 + (c * px - sv * py) / 4096.0,
                        ctr->m2 + (sv * px + c * py) / 4096.0, ctr->m3,
                        v->ofx, v->ofy, v, 0, &o);
        x[j] = (float)o.sx;
        y[j] = (float)o.sy;
    }
    return n;
}

/** @brief Build every paired primitive's corners in float at the tick's own state.
 *
 * The state is the record's position words, spin phase and lifetime byte as
 * the tick read them, under the tick's view; an item glint_corners cannot
 * build keeps exact = 0.
 * @param cpu Reads the routine's tables.
 * @param v The tick's projection view. */
static void glint_build_exact(CPUState* cpu, const ngte_view* v) {
    for (int i = 0; i < s_item_n; i++) {
        glint_item* it = &s_item[i];
        it->exact = 0;
        if (!it->src.addr || (it->p.code != GLINT_QUAD && it->p.code != GLINT_LINE)) continue;
        glint_pose ps = { { 0.0, 0.0, 0.0 }, glint_angle(&it->src), (double)it->src.life, 0.0 };
        for (int k = 0; k < 3; k++) ps.pos[k] = (double)(int32_t)it->src.w[k];
        ngte_proj ctr;
        double b[2];
        if (glint_corners(cpu, v, &it->src, &ps, it->p.code == GLINT_QUAD, it->sub, it->ex, it->ey,
                          it->ec, &ctr, b) == 0)
            continue;
        it->exact = 1;
        it->bre = b[0]; it->bim = b[1];
        it->csx = ctr.sx; it->csy = ctr.sy;
    }
}

/* ---- synthetic stars ------------------------------------------------------
 *
 * Three kinds of record draw primitives the routine did not emit, built by
 * glint_corners exactly as a captured primitive's corners are and appended
 * after the capture as items carrying the record.
 *
 * The widened view's stars: the routine's lateral test 4|X| - 3Z < 0x100 is
 * the 4:3 frame plus a fixed margin, so in a wider view (a Hor+ factor above
 * 1 or a --fov scale below 1, ngte_view.extended) it drops a record whose star still lies inside the revealed side band and
 * emits no primitive for it. Such a record keeps every other verdict of the
 * routine (the lifetime, the box, the depth window and the vertical test) and
 * is admitted unless every corner lies left of the view's screen-X bound xlo
 * or every corner at or right of xhi.
 *
 * Extended LOD's stars: the far branch draws a record at half-world depth
 * GT.near_vz and beyond as a two-line cross, a cheaper picture of the same
 * star. Under extended LOD every record, the widened view's included, is
 * drawn as the near branch's eight quads: its outline, its quad table, its
 * colour words and its additive draw mode, at its own view Z and at the
 * size ramp both branches share (glint_size_ramp). A far record's star keeps
 * the far branch's view-Z scale of the arms (glint_corners), so it spans the
 * screen extent of the cross it replaces: without it the star's extent falls
 * as 1/Z past the switch depth and is under a pixel at the far cull. A captured far record's two lines stay in the
 * capture, marked hidden, so the pairing, calibration and census still read
 * the branch the routine took.
 *
 * Extended LOD's distant stars: the routine drops a live record whose halved
 * offset leaves the +/-0x7800 box or whose half-world view Z reaches 0x7800,
 * bounds of the GTE's 16-bit input range rather than of the star. Extended
 * LOD draws a moby at any distance in front of the camera plane
 * (moby_reveal_extended), and the record names no moby to test against: the
 * spawner keeps the record's slot in its own moby (a byte the routine never
 * reads) and drops that link for a gem past its spawn distance. Such a record
 * therefore keeps every other verdict of the routine (the lifetime, the near
 * depth, the vertical test and the lateral test, or the widened view's corner
 * test in its place) and draws as a far record's star. Its lifetime byte is
 * the routine's own: the routine decrements it before the box test
 * (:2236-2240), so a dropped record ages exactly as a drawn one.
 *
 * A synthetic star takes the routine's draw-mode word 0xE1000620 (:2383),
 * whose rate bits are 0b01. */

#define GLINT_DRMODE_ABR 1   /* (0xE1000620 >> 5) & 3 */

static int s_wide_admit;     /* records the widened view appended this call */
static int s_wide_cull;      /* records the widened view rebuilt and culled this call */
static int s_range_admit;    /* box- or far-culled records extended LOD appended this call */
static int s_range_cull;     /* box- or far-culled records the lateral or corner test culled */

/** @brief Whether the feed draws a record as the near branch's star: its branch is near, or extended LOD is on.
 * @param r The record.
 * @return 1 for the star, 0 for the cross. */
static int glint_draws_star(const glint_rec* r) {
    return r->near_mode || openpete_no_lod;
}

/** @brief Build one record's primitives as synthetic items past the end of s_item without appending them.
 *
 * Each item carries the record, its quad or line index, the corners and
 * colours glint_corners builds at the tick's state, the record's depth and
 * the drawn kind's blend.
 * @param cpu Reads the routine's tables.
 * @param v The tick's projection view.
 * @param r The record.
 * @param star 1 builds the star's eight quads, 0 the cross's two lines.
 * @param xr Receives the smallest and largest corner X.
 * @return The item count, or 0 when s_item has no room for the whole record
 *         or glint_corners builds nothing. */
static int glint_synth_build(CPUState* cpu, const ngte_view* v, const glint_rec* r, int star,
                             float xr[2]) {
    const int n = star ? GLINT_QUADS_PER_STAR : GLINT_LINES_PER_CROSS;
    if (s_item_n + n > GLINT_MAX_PRIMS) return 0;
    glint_pose ps = { { 0.0, 0.0, 0.0 }, glint_angle(r), (double)r->life, 0.0 };
    for (int k = 0; k < 3; k++) ps.pos[k] = (double)(int32_t)r->w[k];
    xr[0] = 1e30f; xr[1] = -1e30f;
    for (int q = 0; q < n; q++) {
        glint_item* it = &s_item[s_item_n + q];
        memset(it, 0, sizeof *it);
        memset(&s_reb[s_item_n + q], 0, sizeof s_reb[0]);
        ngte_proj ctr;
        double b[2];
        const int nc = glint_corners(cpu, v, r, &ps, star, q, it->ex, it->ey, it->ec, &ctr, b);
        if (nc == 0) return 0;
        for (int k = 0; k < nc; k++) {
            if (it->ex[k] < xr[0]) xr[0] = it->ex[k];
            if (it->ex[k] > xr[1]) xr[1] = it->ex[k];
        }
        it->p.code = star ? GLINT_QUAD : GLINT_LINE;
        it->p.nv = star ? 4 : 0;
        it->depth = r->depth;
        it->paired = 1;
        it->blend = star ? ngte_abr_blend(GLINT_DRMODE_ABR, NGTE_ABR_QUARTER_AS_ADD) : 0;
        it->sub = q;
        it->exact = 1;
        it->bre = b[0]; it->bim = b[1];
        it->csx = ctr.sx; it->csy = ctr.sy;
        it->src = *r;
    }
    return n;
}

/** @brief Append the stars the routine dropped but a view wider than 4:3 or extended LOD shows.
 *
 * A record glint_accept flags GLINT_LATERAL is a candidate when the view is
 * wider than 4:3 (ngte_view.extended), and one it flags GLINT_RANGE when extended LOD is
 * on; a record carrying both flags needs both. A candidate is built by
 * glint_synth_build in the kind glint_draws_star picks. One flagged
 * GLINT_LATERAL is appended unless the view shows none of its corners; one
 * flagged GLINT_RANGE alone is appended, the routine's lateral test having
 * passed it. Tallies s_wide_admit and s_wide_cull for the lateral-only
 * candidates and s_range_admit and s_range_cull for the range candidates.
 * @param cpu The guest CPU state, read only.
 * @param v The tick's projection view. */
static void glint_reveal(CPUState* cpu, const ngte_view* v) {
    s_wide_admit = 0; s_wide_cull = 0; s_range_admit = 0; s_range_cull = 0;
    const int wide = v->extended;
    for (uint32_t s = 0; s < GT.slots; s++) {
        glint_rec r;
        const int a = glint_accept(cpu, v, GT.pool + s * GT.stride, &r);
        if (a == 0 || a == 1) continue;
        const int range = (a & GLINT_RANGE) != 0, lateral = (a & GLINT_LATERAL) != 0;
        if (range && !openpete_no_lod) continue;
        if (lateral && !wide) { s_range_cull += range; continue; }
        float xr[2];
        const int n = glint_synth_build(cpu, v, &r, glint_draws_star(&r), xr);
        if (n == 0) continue;
        if (lateral && (xr[1] < v->xlo || xr[0] >= v->xhi)) {
            if (range) s_range_cull++; else s_wide_cull++;
            continue;
        }
        s_item_n += n;
        if (range) s_range_admit++; else s_wide_admit++;
    }
}

/** @brief Under extended LOD, append a star for every captured far record and hide its two lines.
 *
 * Walks the captured primitives for the first line of each paired cross, builds
 * the record's star with glint_synth_build and appends it, then marks every
 * captured line of that record hidden. A record s_item has no room for keeps
 * its cross. Counts the records in s_hq_far.
 * @param cpu Reads the routine's tables.
 * @param v The tick's projection view. */
static void glint_star_far(CPUState* cpu, const ngte_view* v) {
    s_hq_far = 0;
    if (!openpete_no_lod) return;
    for (int i = 0; i < s_cap_n; i++) {
        const glint_item* it = &s_item[i];
        if (it->p.code != GLINT_LINE || !it->src.addr || it->sub != 0 || it->hidden) continue;
        const glint_rec r = it->src;
        float xr[2];
        const int n = glint_synth_build(cpu, v, &r, 1, xr);
        if (n == 0) continue;
        s_item_n += n;
        for (int j = 0; j < s_cap_n; j++)
            if (s_item[j].p.code == GLINT_LINE && s_item[j].src.addr == r.addr) s_item[j].hidden = 1;
        s_hq_far++;
    }
}

/** @brief Whether the sub-tick replay scales a primitive's extent, latched on first use.
 *
 * OPENPETE_GLINT_EXTENT=0 holds the tick's extent on the replay, the
 * engagement leg of the sub-tick extent harness; any other value or none
 * enables the scale.
 * @return 1 when enabled, else 0. */
static int glint_extent_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLINT_EXTENT"); v = !(e && *e == '0'); }
    return v;
}

/** @brief Whether the sub-tick replay rotates a primitive to the shown spin phase, latched on first use.
 *
 * OPENPETE_GLINT_SPIN=0 holds the tick's spin phase on the replay, the
 * engagement leg of the sub-tick phase harness; any other value or none
 * enables the rotation.
 * @return 1 when enabled, else 0. */
static int glint_spin_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLINT_SPIN"); v = !(e && *e == '0'); }
    return v;
}

/* OPENPETE_GLINT_REBASE_DBG=1: the rebase's verdicts per run. A run whose
 * lerped count is zero rebased nothing against a previous record and measured
 * nothing; a snapped primitive had no usable previous record (none, a stale
 * pair, or a slot that took a different gem). */
static unsigned long s_reb_stat[3];   /* 0 lerped, 1 snapped, 2 rebuilt */

/** @brief Print the rebase verdict counters to stderr.
 *
 * Registered with atexit by glint_rebase_all when OPENPETE_GLINT_REBASE_DBG is
 * set. Prints how many primitives took a lerped record pair, how many
 * snapped to the tick's pose, and how many had their corners rebuilt. */
static void glint_rebase_report(void) {
    OP_ROLE_ASSERT(sim);
    fprintf(stderr, "[glint-rebase] lerped=%lu snapped=%lu rebuilt=%lu\n",
            s_reb_stat[0], s_reb_stat[1], s_reb_stat[2]);
}

/** @brief Compute the rebase of one captured primitive onto the pose this present shows.
 *
 * Resets the item's rebase to the identity. A primitive with no owning record,
 * or a hidden one, stays inactive. Otherwise the record's position, spin phase and lifetime at
 * this present come from glint_record_pair (the tick's own values when the
 * pair snaps) and the record point is re-projected under the staged view: the
 * rebase takes the scene depth glint_star_vz gives at that point (the record
 * plus its owner vector locating the owner) and its screen displacement from
 * the tick's projection, and glint_corners rebuilds the primitive's corners
 * at the shown state. At the end of the tick's interval the shown state is
 * the tick's and the corners equal the ones glint_build_exact built.
 * OPENPETE_GLINT_SPIN=0 holds the tick's phase; OPENPETE_GLINT_EXTENT=0 holds
 * the tick's lifetime and, for a near star, the tick's view-Z in the extent,
 * so the star keeps the screen extent the tick drew. Updates the s_reb_stat
 * counters.
 * @param cpu The guest CPU state, read only.
 * @param v The projection view of this present.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @param i Index of the primitive in s_item and s_reb. */
static void glint_rebase_item(CPUState* cpu, const ngte_view* v, double alpha, int i) {
    glint_item* it = &s_item[i];
    glint_rebase* r = &s_reb[i];
    memset(r, 0, sizeof *r);
    if (!it->src.addr || it->hidden) return;     /* unpaired, or not drawn */
    double off[3], show_ang = 0.0, show_life = 0.0;
    const int paired = glint_record_pair(&it->src, alpha, off, &show_ang, &show_life);
    if (!glint_spin_enabled()) show_ang = glint_angle(&it->src);
    r->ang = show_ang;
    s_reb_stat[paired ? 0 : 1]++;
    glint_pose ps = { { 0.0, 0.0, 0.0 }, show_ang, show_life, 0.0 };
    for (int k = 0; k < 3; k++) ps.pos[k] = (double)(int32_t)it->src.w[k] + off[k];
    if (!glint_extent_enabled()) { ps.life = (double)it->src.life; ps.hold_vz = it->src.pvz; }
    double vo[3];
    glint_view_offset(v, ps.pos, 0, vo);
    ngte_proj o;
    glint_project(v, vo, &o);
    r->dx = (float)(o.sx - it->src.psx);
    r->dy = (float)(o.sy - it->src.psy);
    r->depth = openpete_native_scene_viewz(
        openpete_worldvz_scaled((float)glint_star_vz(v, ps.pos, o.m3, &it->src), 2.0f));
    r->active = 1;
    if (!it->exact) return;
    uint32_t rgb[4];
    ngte_proj ctr;
    double b[2];
    r->corners = glint_corners(cpu, v, &it->src, &ps, it->p.code == GLINT_QUAD, it->sub, r->cx,
                               r->cy, rgb, &ctr, b) > 0;
    r->bre = b[0]; r->bim = b[1];
    r->csx = ctr.sx; r->csy = ctr.sy;
    s_reb_stat[2] += r->corners;
}

/** @brief Rebase every captured primitive onto the pose this present shows.
 *
 * The staged camera resolves through the sub-tick view, so building a view
 * here reads the interpolated transform the rest of the pass draws under.
 * OPENPETE_GLINT_REBASE_DBG, latched on first call, registers
 * glint_rebase_report at exit.
 * @param cpu The guest CPU state, read only.
 * @param alpha The present's position in the tick interval, 1 at its end. */
static void glint_rebase_all(CPUState* cpu, double alpha) {
    if (s_item_n <= 0) return;
    static int dbg = -1;
    if (dbg < 0) {
        const char* e = getenv("OPENPETE_GLINT_REBASE_DBG");
        dbg = (e && *e && *e != '0');
        if (dbg) atexit(glint_rebase_report);
    }
    ngte_view v;
    ngte_view_build(cpu, &v);
    for (int i = 0; i < s_item_n; i++) glint_rebase_item(cpu, &v, alpha, i);
}

/** @brief Translate one captured corner by its primitive's rebase displacement.
 *
 * Serves a primitive glint_corners could not build; an inactive rebase leaves
 * the corner unchanged.
 * @param r The primitive's rebase.
 * @param x The corner's screen X, moved in place.
 * @param y The corner's screen Y, moved in place. */
static void glint_rebase_xy(const glint_rebase* r, float* x, float* y) {
    if (!r->active) return;
    *x += r->dx;
    *y += r->dy;
}

/** @brief Read a captured primitive's corners and colours as the routine wrote them.
 * @param it The captured primitive, a star quad or a cross line.
 * @param x Receives the corner X.
 * @param y Receives the corner Y.
 * @param rgb Receives the corner colours, low 24 bits.
 * @return The corner count, 4 or 2, or 0 for any other primitive. */
static int glint_guest_corners(const glint_item* it, float x[4], float y[4], uint32_t rgb[4]) {
    const ngte_prim* p = &it->p;
    if (p->code == GLINT_QUAD && p->nv == 4) {
        for (int k = 0; k < 4; k++) { x[k] = p->x[k]; y[k] = p->y[k]; rgb[k] = p->rgb[k] & 0xFFFFFFu; }
        return 4;
    }
    if (p->code != GLINT_LINE) return 0;
    for (int k = 0; k < 2; k++) {
        const uint32_t w = p->raw[2 + k];
        x[k] = (float)(int16_t)(w & 0xFFFFu);
        y[k] = (float)(int16_t)(w >> 16);
        rgb[k] = p->raw[1] & 0xFFFFFFu;
    }
    return 2;
}

/** @brief The corners and colours the feed draws a star quad or a cross line at.
 *
 * In order of precedence: the corners the rebase rebuilt at the shown state,
 * the corners glint_build_exact built at the tick's state, or the captured
 * corners translated by the rebase. Colours are the colour table's when the
 * corners were built, else the captured ones.
 * @param it The captured primitive.
 * @param rb Its rebase.
 * @param x Receives the corner X.
 * @param y Receives the corner Y.
 * @param rgb Receives the corner colours, low 24 bits.
 * @return The corner count, 4 for a quad and 2 for a line, else 0. */
static int glint_fed_corners(const glint_item* it, const glint_rebase* rb, float x[4], float y[4],
                             uint32_t rgb[4]) {
    if (!it->exact) {
        const int n = glint_guest_corners(it, x, y, rgb);
        for (int k = 0; k < n; k++) glint_rebase_xy(rb, &x[k], &y[k]);
        return n;
    }
    const int n = it->p.code == GLINT_QUAD ? 4 : 2;
    const int shown = rb->active && rb->corners;
    memcpy(x, shown ? rb->cx : it->ex, (size_t)n * sizeof x[0]);
    memcpy(y, shown ? rb->cy : it->ey, (size_t)n * sizeof y[0]);
    memcpy(rgb, it->ec, (size_t)n * sizeof rgb[0]);
    return n;
}

/** @brief Push every captured primitive to the native scene at the corners glint_fed_corners gives.
 *
 * Does nothing when OPENPETE_GLINT selects 0 or nothing was captured, and
 * skips a hidden primitive (glint_star_far). Each star quad (code GLINT_QUAD) goes through ngte_feed_flat_quad; with mode 2 or
 * more each cross line goes through ngte_feed_flat_line (centre 0.5 on the
 * captured integer corners, 0 on built ones), which widens it to
 * the one pixel the GPU rasterizes a line at. The depth is the rebase's when
 * active, else the depth the pairing gave. Counts the triangles fed in
 * s_fed_star_tris and s_fed_cross_tris and tags them as flare and spark
 * particles with sel 0, since neither kind carries a game particle class. */
static void glint_feed_native(void) OP_REQUIRES(sim) {
    const int mode = glint_feed_mode();
    if (mode <= 0) return;
    if (s_item_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_item_n; i++) {
        const glint_item* it = &s_item[i];
        const glint_rebase* rb = &s_reb[i];
        if (it->hidden) continue;
        const float depth = rb->active ? rb->depth : it->depth;
        const int quad = it->p.code == GLINT_QUAD && it->p.nv == 4;
        if (!quad && !(it->p.code == GLINT_LINE && mode >= 2)) continue;
        float x[4], y[4];
        uint32_t c[4];
        (void)glint_fed_corners(it, rb, x, y, c);
        if (quad) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_FLARE, 0, 0));
            static const float zu[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            (void)ngte_feed_flat_quad(x, y, zu, zu, c, depth, NULL, it->blend, 4);
            s_fed_star_tris += 2;
        } else {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_SPARK, 0, 0));
            (void)ngte_feed_flat_line(x[0], y[0], x[1], y[1], c[0], c[1],
                                      it->exact ? 0.0f : 0.5f, depth, it->blend);
            s_fed_cross_tris += 2;
        }
    }
    openpete_native_scene_set_tag(0);
}

/* One record's accumulator in the trace below: the extent sum, the corner
 * centroid, and the fourth-power moment of the corner offsets. */
typedef struct {
    uint32_t addr; double sum; int n; double px, py, cx, cy;
    double mre, mim;   /* sum of (dx + i*dy)^4 over the corners */
    double ph;         /* the spin phase the feed rotated the record to */
    int    life;       /* the tick's lifetime byte */
    int    near_mode;  /* the branch the tick drew the record with */
    double vz;         /* the tick's half-world view-Z of the record point */
} glint_ext_acc;

/** @brief Add one primitive's outline corners to its record's trace accumulator.
 *
 * The record point is the one this present shows (the tick's projection
 * plus the rebase displacement) and the corners are the ones the feed draws
 * (glint_fed_corners). Adds each corner's distance from that point, the corner
 * coordinates, and the fourth power of each corner offset as a complex
 * number; overwrites the point, the shown spin phase, the lifetime byte, the
 * branch and the view Z with this primitive's.
 * @param it The paired primitive.
 * @param rb Its rebase; inactive leaves the tick's positions and phase.
 * @param acc The record's accumulator. */
static void glint_ext_add(const glint_item* it, const glint_rebase* rb, glint_ext_acc* acc) {
    /* The outline corners: a star quad's rim corners 0, 1 and 3 (corner 2 is
     * the shared centre), a cross line's two endpoints. */
    static const int rim[3] = { 0, 1, 3 };
    float fx[4], fy[4], cx[3], cy[3];
    uint32_t rgb[4];
    const int nf = glint_fed_corners(it, rb, fx, fy, rgb);
    const int nc = nf == 4 ? 3 : nf;
    for (int k = 0; k < nc; k++) {
        cx[k] = fx[nf == 4 ? rim[k] : k];
        cy[k] = fy[nf == 4 ? rim[k] : k];
    }
    const double px = it->src.psx + (rb->active ? rb->dx : 0.0f);
    const double py = it->src.psy + (rb->active ? rb->dy : 0.0f);
    acc->px = px;
    acc->py = py;
    acc->ph = rb->active ? rb->ang : glint_angle(&it->src);
    acc->life = it->src.life;
    acc->near_mode = it->src.near_mode;
    acc->vz = it->src.pvz;
    for (int k = 0; k < nc; k++) {
        const double dx = (double)cx[k] - px, dy = (double)cy[k] - py;
        acc->sum += hypot(dx, dy);
        acc->cx += cx[k];
        acc->cy += cy[k];
        const double re2 = dx * dx - dy * dy, im2 = 2.0 * dx * dy;
        acc->mre += re2 * re2 - im2 * im2;
        acc->mim += 2.0 * re2 * im2;
        acc->n++;
    }
}

/** @brief The drawn orientation of one record's outline, in sine-table entries (256 to the turn) modulo 64.
 *
 * The star's four spikes and the cross's four arms repeat every quarter
 * turn, so the argument of the summed fourth power of the corner offsets,
 * over four, is the outline's angle; the fourth power weights the spike tips,
 * the outermost corners, above the shoulder points between them.
 * @param a The record's accumulator.
 * @return The angle in [0, 64), or 0 when the moment is zero. */
static double glint_drawn_orientation(const glint_ext_acc* a) {
    if (a->mre == 0.0 && a->mim == 0.0) return 0.0;
    const double t = atan2(a->mim, a->mre) * (32.0 / 3.14159265358979323846);
    return fmod(t + 64.0, 64.0);
}

/** @brief Print the extent and orientation trace of every paired record this present fed.
 *
 * OPENPETE_GLINT_TRACE, latched on first call, enables it when set, non-empty
 * and not starting with '0': two lines per paired record per present that fed
 * this producer. `[glint-ext]` carries the record's address and the extent the
 * feed drew it at: the mean distance of its outline corners (a star quad's
 * three rim corners, a cross line's two endpoints) from its projected point,
 * both as the feed placed them. `[glint-rot]` carries the outline's drawn
 * orientation (glint_drawn_orientation) and the spin phase the feed asked for,
 * the routine's phase on a canonical present and the rebase's shown phase on a
 * sub-tick one. The corners are the ones the feed draws (glint_fed_corners).
 * @param leg The present kind printed on each line, "canon" or "inter".
 * @param alpha The present's position in the tick interval, printed. */
static void glint_trace(const char* leg, double alpha) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_GLINT_TRACE"); on = (e && *e && *e != '0'); }
    if (!on) return;
    glint_ext_acc acc[GLINT_MAX_PRIMS];
    int nrec = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (!s_item[i].src.addr || s_item[i].hidden) continue;
        int r = 0;
        while (r < nrec && acc[r].addr != s_item[i].src.addr) r++;
        if (r == nrec) {
            memset(&acc[r], 0, sizeof acc[r]);
            acc[r].addr = s_item[i].src.addr;
            nrec++;
        }
        glint_ext_add(&s_item[i], &s_reb[i], &acc[r]);
    }
    for (int r = 0; r < nrec; r++) {
        if (acc[r].n <= 0) continue;
        fprintf(stderr, "[glint-ext vbl=%u leg=%s alpha=%.3f] rec=%08X ext=%.4f n=%d"
                        " at=(%.2f,%.2f) c=(%.3f,%.3f)\n",
                openpete_feed_key_now(), leg, alpha, acc[r].addr, acc[r].sum / (double)acc[r].n,
                acc[r].n, acc[r].px, acc[r].py, acc[r].cx / (double)acc[r].n,
                acc[r].cy / (double)acc[r].n);
        fprintf(stderr, "[glint-rot vbl=%u leg=%s alpha=%.3f] rec=%08X ori=%.4f ph=%.4f"
                        " ext=%.4f n=%d life=%d near=%d vz=%.1f\n",
                openpete_feed_key_now(), leg, alpha, acc[r].addr, glint_drawn_orientation(&acc[r]),
                acc[r].ph, acc[r].sum / (double)acc[r].n, acc[r].n, acc[r].life,
                acc[r].near_mode, acc[r].vz);
    }
}

/** @brief Check one star's captured geometry for a shared centre corner and count its distinct rim points.
 *
 * The star must be eight four-vertex quads sharing one centre corner (corner
 * 2); their other corners are the rim points of the outline the routine
 * transformed, sixteen at most. A decode that mis-slices the quad words, or a
 * walk that straddles a primitive boundary, breaks the shared centre. A small
 * star on screen quantizes neighbouring rim points onto the same pixel, so
 * fewer than sixteen is normal.
 * @param first Index in s_item of the star's first quad.
 * @param count The number of quads the star holds.
 * @return The number of distinct rim points, or -1 when the quad count is not
 *         eight, a quad is not four-vertex, or the centre is not shared. */
static int glint_star_topology_ok(int first, int count) {
    if (count != GLINT_QUADS_PER_STAR) return -1;
    float rx[GLINT_QUADS_PER_STAR * 3], ry[GLINT_QUADS_PER_STAR * 3];
    int rim = 0;
    for (int q = 0; q < count; q++) {
        const ngte_prim* p = &s_item[first + q].p;
        if (p->nv != 4) return -1;
        if (p->x[2] != s_item[first].p.x[2] || p->y[2] != s_item[first].p.y[2]) return -1;
        static const int corner[3] = { 0, 1, 3 };
        for (int k = 0; k < 3; k++) {
            const float cx = p->x[corner[k]], cy = p->y[corner[k]];
            int seen = 0;
            for (int j = 0; j < rim; j++) if (rx[j] == cx && ry[j] == cy) { seen = 1; break; }
            if (!seen && rim < (int)(sizeof rx / sizeof rx[0])) { rx[rim] = cx; ry[rim] = cy; rim++; }
        }
    }
    return rim;
}

/** @brief Count the call's stars that pass glint_star_topology_ok and the largest rim count any reached.
 *
 * A star is the run of quads after one draw-mode packet, up to the next
 * packet or the end of the capture.
 * @param ok Receives the number of passing stars.
 * @param rim_max Receives the largest distinct rim count of a passing star. */
static void glint_stars_ok(int* ok, int* rim_max) {
    int first = -1, n = 0;
    *ok = 0; *rim_max = 0;
    for (int i = 0; i <= s_cap_n; i++) {
        const int end = (i == s_cap_n) || s_item[i].p.code == GLINT_DRMODE;
        if (end) {
            if (first >= 0) {
                const int rim = glint_star_topology_ok(first, n);
                if (rim >= 0) { (*ok)++; if (rim > *rim_max) *rim_max = rim; }
            }
            first = i + 1; n = 0;
        } else if (s_item[i].p.code == GLINT_QUAD && first >= 0) {
            n++;
        }
    }
}

/** @brief Measure how far each paired primitive's record point lands from the point the routine drew it about.
 *
 * The reference point is a star's shared centre corner, taken once per run of
 * quads, or a cross line's midpoint, taken per line; the distance is the
 * Manhattan distance in pixels. The sub-tick rotation pivots on the projected
 * point, so this bounds the error that pivot introduces; a residual that is
 * not a fraction of a pixel means the walk is projecting a different record
 * than the one that emitted the primitive.
 * @param mean Receives the mean distance, 0 when nothing was measured.
 * @param worst Receives the largest distance.
 * @param n Receives the number of points measured. */
static void glint_pivot_residual(double* mean, double* worst, int* n) {
    double sum = 0.0;
    *worst = 0.0; *n = 0;
    for (int i = 0; i < s_cap_n; i++) {
        const glint_item* it = &s_item[i];
        if (!it->src.addr) continue;
        double gx, gy;
        if (it->p.code == GLINT_QUAD && it->p.nv == 4) {
            if (i > 0 && s_item[i - 1].p.code == GLINT_QUAD) continue;  /* one per star */
            gx = (double)it->p.x[2]; gy = (double)it->p.y[2];
        } else if (it->p.code == GLINT_LINE) {
            const uint32_t a = it->p.raw[2], b = it->p.raw[3];
            gx = ((double)(int16_t)(a & 0xFFFFu) + (double)(int16_t)(b & 0xFFFFu)) / 2.0;
            gy = ((double)(int16_t)(a >> 16) + (double)(int16_t)(b >> 16)) / 2.0;
        } else {
            continue;
        }
        const double d = fabs(gx - it->src.psx) + fabs(gy - it->src.psy);
        sum += d;
        if (d > *worst) *worst = d;
        (*n)++;
    }
    *mean = *n ? sum / (double)*n : 0.0;
}

/** @brief Print the live pool records the routine's box or far-depth cull drops, which no branch draws.
 *
 * One `[glint-cull]` line with the live record count (lifetime byte nonzero)
 * and how many of them leave the +/-GT.box box or stand at half-world view Z
 * GT.far_z or beyond, each on the floored offset the routine tests
 * (glint_accept), then one line per such record with its address, its view Z
 * and its world distance from the camera.
 * @param cpu The guest CPU state, read only.
 * @param key The VBL key the lines are labelled with. */
static void glint_cull_census(CPUState* cpu, unsigned key) {
    ngte_view v;
    ngte_view_build(cpu, &v);
    int live = 0, box_n = 0, far_n = 0;
    char detail[16][96];
    int nd = 0;
    for (uint32_t s = 0; s < GT.slots; s++) {
        const uint32_t rec = GT.pool + s * GT.stride;
        if ((cpu->read_word(rec + GT.packed_off) & 0xFFu) == 0u) continue;
        live++;
        double p[3], vo[3];
        for (int k = 0; k < 3; k++) p[k] = (double)(int32_t)cpu->read_word(rec + GT.pos_off + (uint32_t)k * 4u);
        glint_view_offset(&v, p, 1, vo);
        int out_box = 0;
        for (int k = 0; k < 3; k++) if (vo[k] >= (double)GT.box || vo[k] <= -(double)GT.box) out_box = 1;
        ngte_proj o;
        glint_project(&v, vo, &o);
        const int out_far = !out_box && o.m3 >= (double)GT.far_z;
        if (!out_box && !out_far) continue;
        box_n += out_box; far_n += out_far;
        if (nd < 16)
            snprintf(detail[nd++], sizeof detail[0], "rec=%08X %s vz=%.0f dist=%.0f", rec,
                     out_box ? "box" : "far", o.m3, 2.0 * sqrt(vo[0] * vo[0] + vo[1] * vo[1] + vo[2] * vo[2]));
    }
    fprintf(stderr, "[glint-cull vbl=%u] live=%d box=%d far=%d\n", key, live, box_n, far_n);
    for (int i = 0; i < nd; i++) fprintf(stderr, "[glint-cull vbl=%u] %s\n", key, detail[i]);
}

/* The owner census's run totals: records owned, owned and pulled nearer,
 * unowned, and the largest pull in world units. */
static unsigned long s_own_tot[3];
static double        s_own_tot_max;

/** @brief Print the owner census's run totals to stderr; registered with atexit by glint_own_census. */
static void glint_own_report(void) {
    OP_ROLE_ASSERT(sim);
    fprintf(stderr, "[glint-own total] owned=%lu pulled=%lu unowned=%lu max_pull=%.1f\n",
            s_own_tot[0], s_own_tot[1], s_own_tot[2], s_own_tot_max);
}

/** @brief Print one `[glint-own]` line over the records this call fed, and add them to the run totals.
 *
 * Each record the set draws counts once: owned (a candidate lay within the
 * sparkle reach), of those pulled (its owner's sphere front was nearer than
 * the record and set the depth), or unowned; max_pull is the largest pull in
 * world units. A run whose pulled count is zero exercised no pull.
 * @param key The VBL key the line is labelled with. */
static void glint_own_census(unsigned key) {
    static int armed = 0;
    if (!armed) { armed = 1; atexit(glint_own_report); }
    uint32_t seen[GLINT_MAX_PRIMS];
    int nseen = 0, owned = 0, pulled = 0, unowned = 0;
    double max_pull = 0.0;
    for (int i = 0; i < s_item_n; i++) {
        const glint_rec* r = &s_item[i].src;
        if (!r->addr || s_item[i].hidden) continue;
        int dup = 0;
        for (int k = 0; k < nseen && !dup; k++) dup = seen[k] == r->addr;
        if (dup) continue;
        seen[nseen++] = r->addr;
        if (r->own_rad <= 0.0f) { unowned++; continue; }
        owned++;
        if (r->pull > 0.0f) { pulled++; max_pull = fmax(max_pull, (double)r->pull); }
    }
    s_own_tot[0] += (unsigned long)owned;
    s_own_tot[1] += (unsigned long)pulled;
    s_own_tot[2] += (unsigned long)unowned;
    s_own_tot_max = fmax(s_own_tot_max, max_pull);
    fprintf(stderr, "[glint-own vbl=%u] owned=%d pulled=%d unowned=%d max_pull=%.1f candidates=%d reach=%.1f\n",
            key, owned, pulled, unowned, max_pull, s_own_n, s_own_reach);
}

/** @brief Print the per-call glint census when the VBL lies in the OPENPETE_GLINT_CENSUS range.
 *
 * OPENPETE_GLINT_CENSUS=LO-HI (or a single VBL), on the VBL key
 * openpete_feed_key_now() that --screenshot-on-frame takes: per call in that range, the
 * primitives the routine appended by GPU code, the stars its draw-mode packets
 * delimit and the crosses its line pairs delimit, the stars passing the
 * topology check, the near and far records the pool walk predicted, the
 * records the widened view appended and culled and the box- or far-culled
 * records extended LOD appended and culled (glint_reveal), the far
 * records drawn as stars under extended LOD (glint_star_far), the
 * native triangles this feeder emitted from each class, how many primitives
 * were left unpaired, the pairing's pivot residual, and the run's totals of
 * groups bound in emission order, bound to their nearest record
 * (glint_pair_nearest) and left at depth 1; then one line per star
 * centre and per cross first endpoint with its paired depth. The live records
 * the routine culls on its box or far depth print first (glint_cull_census),
 * then the owner census (glint_own_census).
 * A capture whose fed count is zero measured nothing.
 * @param cpu The guest CPU state, read only.
 * @param start First polybuf address of the routine's output.
 * @param end One past the last.
 * @param star_before The fed star triangle total before this call.
 * @param cross_before The fed cross triangle total before this call. */
static void glint_census(CPUState* cpu, uint32_t start, uint32_t end,
                         uint32_t star_before, uint32_t cross_before) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    const unsigned key = openpete_feed_key_now();
    if (!env_frame_range_active(&s_range, "OPENPETE_GLINT_CENSUS", key)) return;
    glint_cull_census(cpu, key);
    glint_own_census(key);
    int unpaired = 0;
    for (int i = 0; i < s_cap_n; i++)
        if (s_item[i].p.code != GLINT_DRMODE && !s_item[i].paired) unpaired++;
    fprintf(stderr, "[glint-census vbl=%u] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            key, start, end, end - start, s_cap_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_cap_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    int stars_ok = 0, rim_max = 0;
    glint_stars_ok(&stars_ok, &rim_max);
    double pmean, pworst; int pn;
    glint_pivot_residual(&pmean, &pworst, &pn);
    fprintf(stderr, " stars=%d centre_ok=%d rim_max=%d crosses=%d pred_near=%d pred_far=%d"
                    " wide_admit=%d wide_cull=%d range_admit=%d range_cull=%d hq_far=%d fed_star_tris=%u fed_cross_tris=%u unpaired=%d"
                    " pivot:n=%d mean=%.2f worst=%.2f total order=%lu nearest=%lu depth1=%lu\n",
            s_stars, stars_ok, rim_max, s_crosses, s_pred_near, s_pred_far,
            s_wide_admit, s_wide_cull, s_range_admit, s_range_cull, s_hq_far,
            s_fed_star_tris - star_before, s_fed_cross_tris - cross_before, unpaired,
            pn, pmean, pworst, s_glint_order, s_glint_nearest, s_glint_depth1);
    /* Each star's centre corner and each cross's first endpoint, with the scene
     * depth the pairing gave it: the two quantities the feed places them by. */
    int line_i = 0;
    for (int i = 0; i < s_cap_n; i++) {
        if (s_item[i].p.code == GLINT_QUAD &&
            (i == 0 || s_item[i - 1].p.code != GLINT_QUAD)) {
            fprintf(stderr, "[glint-star vbl=%u] centre=(%.0f,%.0f) depth=%.6f blend=%d\n",
                    key, s_item[i].p.x[2], s_item[i].p.y[2],
                    s_item[i].depth, s_item[i].blend);
        } else if (s_item[i].p.code == GLINT_LINE) {
            if (line_i % GLINT_LINES_PER_CROSS == 0)
                fprintf(stderr, "[glint-cross vbl=%u] a=(%d,%d) depth=%.6f\n", key,
                        (int)(int16_t)(s_item[i].p.raw[2] & 0xFFFFu),
                        (int)(int16_t)(s_item[i].p.raw[2] >> 16), s_item[i].depth);
            line_i++;
        }
    }
}

/* ---- per-vertex placement probe -------------------------------------------
 *
 * OPENPETE_GLINT_VTX=1 prints at exit, =2 also per tick and per present:
 *   * cal: each captured corner minus the corner glint_corners builds at the
 *     tick's own state, per axis, and how many corner colours disagree. The
 *     GTE floors every MAC and the screen divide, so a correct model reads in
 *     (-1, 0] with a mean near -0.5 that moves from tick to tick.
 *   * place: the distance of each corner the feed draws from the corner
 *     glint_corners builds at the state the present shows, canonical and
 *     sub-tick presents apart.
 *   * shape: per record, the corners' offsets from the record point against
 *     the previous present's offsets carried by the change of the outline
 *     basis (spin and extent); the largest residual per present, presents
 *     that open a tick and sub-tick presents apart, skipping a record whose
 *     drawn kind changed (a cross and a star share no outline). A shape that
 *     changes only by its spin and extent reads 0. */

typedef struct { unsigned long n; double sum, mn, mx; } glint_acc1;

/** @brief Add one sample to a running count, sum, minimum and maximum.
 * @param a The accumulator.
 * @param d The sample. */
static void glint_acc1_add(glint_acc1* a, double d) {
    if (a->n == 0 || d < a->mn) a->mn = d;
    if (a->n == 0 || d > a->mx) a->mx = d;
    a->sum += d;
    a->n++;
}

static glint_acc1 s_vtx_cal[2];      /* captured minus model, X and Y */
static glint_acc1 s_vtx_tick[2];     /* the per-tick mean of the above */
static unsigned long s_vtx_rgb_bad;  /* corner colours the model disagrees on */
static glint_acc1 s_vtx_place[2];    /* fed minus model distance, canonical and sub-tick */
static glint_acc1 s_vtx_shape[2];    /* shape residual per present, canonical and sub-tick */

#define GLINT_SHAPE_CORNERS 32

/* One record's corner offsets from its record point as one present drew them. */
typedef struct {
    uint32_t addr;
    int      near_mode;  /* 1 = the present drew the record as a star, 0 as a cross */
    double   bre, bim;
    float    ox[GLINT_SHAPE_CORNERS], oy[GLINT_SHAPE_CORNERS];
    uint32_t have;
} glint_shape;

static glint_shape s_shape_prev[GLINT_MAX_PRIMS];
static int         s_shape_prev_n;

/** @brief OPENPETE_GLINT_VTX, latched on first use: 0 off, 1 the exit summary, 2 also the per-tick and per-present lines.
 * @return The level. */
static int glint_vtx_level(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_GLINT_VTX"); v = (e && *e) ? atoi(e) : 0; }
    return v;
}

/** @brief Print the probe's summary to stderr; registered with atexit. */
static void glint_vtx_report(void) {
    OP_ROLE_ASSERT(sim);
    static const char* const ax[2] = { "x", "y" };
    for (int k = 0; k < 2; k++) {
        const glint_acc1* a = &s_vtx_cal[k];
        const glint_acc1* t = &s_vtx_tick[k];
        fprintf(stderr, "[glint-vtx cal %s] n=%lu mean=%.4f min=%.4f max=%.4f"
                        " tick_mean: n=%lu min=%.4f max=%.4f\n",
                ax[k], a->n, a->n ? a->sum / (double)a->n : 0.0, a->mn, a->mx,
                t->n, t->mn, t->mx);
    }
    fprintf(stderr, "[glint-vtx cal rgb] mismatched=%lu\n", s_vtx_rgb_bad);
    static const char* const leg[2] = { "canon", "inter" };
    for (int k = 0; k < 2; k++)
        fprintf(stderr, "[glint-vtx place %s] n=%lu mean=%.6f max=%.6f\n", leg[k],
                s_vtx_place[k].n, s_vtx_place[k].n ? s_vtx_place[k].sum / (double)s_vtx_place[k].n : 0.0,
                s_vtx_place[k].mx);
    for (int k = 0; k < 2; k++)
        fprintf(stderr, "[glint-vtx shape %s] presents=%lu mean=%.6f max=%.6f\n", leg[k],
                s_vtx_shape[k].n, s_vtx_shape[k].n ? s_vtx_shape[k].sum / (double)s_vtx_shape[k].n : 0.0,
                s_vtx_shape[k].mx);
}

/** @brief Latch the probe and register its summary on first use.
 * @return The level, 0 when the probe is off. */
static int glint_vtx_on(void) {
    static int armed = 0;
    const int lv = glint_vtx_level();
    if (lv > 0 && !armed) { armed = 1; atexit(glint_vtx_report); }
    return lv;
}

/** @brief Calibrate glint_corners against the tick's captured corners: the probe's cal statistic. */
static void glint_vtx_cal(void) {
    if (!glint_vtx_on()) return;
    double sx = 0.0, sy = 0.0;
    int n = 0;
    for (int i = 0; i < s_item_n; i++) {
        const glint_item* it = &s_item[i];
        if (!it->exact) continue;
        float gx[4], gy[4];
        uint32_t rgb[4];
        const int nc = glint_guest_corners(it, gx, gy, rgb);
        for (int k = 0; k < nc; k++) {
            const double dx = (double)gx[k] - (double)it->ex[k];
            const double dy = (double)gy[k] - (double)it->ey[k];
            glint_acc1_add(&s_vtx_cal[0], dx);
            glint_acc1_add(&s_vtx_cal[1], dy);
            if (rgb[k] != it->ec[k]) s_vtx_rgb_bad++;
            sx += dx; sy += dy; n++;
        }
    }
    if (n == 0) return;
    glint_acc1_add(&s_vtx_tick[0], sx / n);
    glint_acc1_add(&s_vtx_tick[1], sy / n);
    if (glint_vtx_level() >= 2)
        fprintf(stderr, "[glint-vtx-cal vbl=%u] n=%d mean=(%.4f,%.4f)\n", openpete_feed_key_now(), n,
                sx / n, sy / n);
}

/** @brief Find or add a record's entry in a shape table.
 * @param t The table.
 * @param n Its entry count, advanced on an add.
 * @param addr The record address.
 * @return The entry, or NULL when the record is absent and add is 0 or the table is full.
 * @param add 1 to add a missing record. */
static glint_shape* glint_shape_find(glint_shape* t, int* n, uint32_t addr, int add) {
    for (int i = 0; i < *n; i++) if (t[i].addr == addr) return &t[i];
    if (!add || *n >= GLINT_MAX_PRIMS) return NULL;
    glint_shape* e = &t[(*n)++];
    memset(e, 0, sizeof *e);
    e->addr = addr;
    return e;
}

/** @brief Add one fed primitive to the probe: its place distances and its corner offsets in its record's shape entry.
 *
 * The model is the corners glint_corners built at the state this present
 * shows: the rebase's when it rebuilt them, else the capture's.
 * @param it The primitive, built in float (exact).
 * @param rb Its rebase.
 * @param leg 0 for a canonical present, 1 for a sub-tick one.
 * @param e The record's shape entry for this present. */
static void glint_vtx_item(const glint_item* it, const glint_rebase* rb, int leg, glint_shape* e) {
    const glint_rebase* m = rb->active && rb->corners ? rb : NULL;
    const float* mx = m ? m->cx : it->ex;
    const float* my = m ? m->cy : it->ey;
    const double cx = m ? m->csx : it->csx, cy = m ? m->csy : it->csy;
    e->near_mode = it->p.code == GLINT_QUAD;
    e->bre = m ? m->bre : it->bre;
    e->bim = m ? m->bim : it->bim;
    float fx[4], fy[4];
    uint32_t rgb[4];
    const int nc = glint_fed_corners(it, rb, fx, fy, rgb);
    for (int k = 0; k < nc; k++) {
        glint_acc1_add(&s_vtx_place[leg], hypot((double)fx[k] - mx[k], (double)fy[k] - my[k]));
        const int slot = (it->sub & 7) * 4 + k;
        e->ox[slot] = (float)(fx[k] - cx);
        e->oy[slot] = (float)(fy[k] - cy);
        e->have |= 1u << slot;
    }
}

/** @brief The largest distance of a record's corner offsets from the previous present's offsets carried by the change of the outline basis.
 * @param c The record's entry this present.
 * @param p Its entry the previous present.
 * @return The residual in pixels, or -1 when the drawn kind changed or the
 *         previous basis is zero. */
static double glint_shape_residual(const glint_shape* c, const glint_shape* p) {
    const double den = p->bre * p->bre + p->bim * p->bim;
    if (!(den > 0.0) || p->near_mode != c->near_mode) return -1.0;
    /* The outline basis ratio B_now / B_prev as a complex number. */
    const double qre = (c->bre * p->bre + c->bim * p->bim) / den;
    const double qim = (c->bim * p->bre - c->bre * p->bim) / den;
    const uint32_t both = c->have & p->have;
    double worst = 0.0;
    for (int k = 0; k < GLINT_SHAPE_CORNERS; k++) {
        if (!(both >> k & 1u)) continue;
        const double ex = p->ox[k] * qre - p->oy[k] * qim;
        const double ey = p->ox[k] * qim + p->oy[k] * qre;
        worst = fmax(worst, hypot(c->ox[k] - ex, c->oy[k] - ey));
    }
    return worst;
}

/** @brief Measure one present's fed corners against the model: the probe's place and shape statistics.
 * @param leg 0 for a canonical present, 1 for a sub-tick one. */
static void glint_vtx_present(int leg) {
    const int lv = glint_vtx_on();
    if (!lv) return;
    static glint_shape cur[GLINT_MAX_PRIMS];
    int ncur = 0;
    for (int i = 0; i < s_item_n; i++) {
        glint_shape* e = s_item[i].exact && !s_item[i].hidden ? glint_shape_find(cur, &ncur, s_item[i].src.addr, 1) : NULL;
        if (e) glint_vtx_item(&s_item[i], &s_reb[i], leg, e);
    }
    for (int r = 0; r < ncur; r++) {
        const glint_shape* p = glint_shape_find(s_shape_prev, &s_shape_prev_n, cur[r].addr, 0);
        const double worst = p ? glint_shape_residual(&cur[r], p) : -1.0;
        if (worst < 0.0) continue;
        glint_acc1_add(&s_vtx_shape[leg], worst);
        if (lv >= 2)
            fprintf(stderr, "[glint-vtx-shape vbl=%u leg=%s] rec=%08X residual=%.4f\n",
                    openpete_feed_key_now(), leg ? "inter" : "canon", cur[r].addr, worst);
    }
    memcpy(s_shape_prev, cur, (size_t)ncur * sizeof cur[0]);
    s_shape_prev_n = ncur;
}

/* ---- hook ---------------------------------------------------------------- */

static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_SPARKLE, .prof_tag = "glint.srend", .st = &s_hb,
};

/** @brief Dispatch hook on the glint routine: run it once through the shared bracket, then capture, pair and feed its primitives.
 *
 * The world draw's call is the only one in either image, so the hook takes
 * every entry (Spyro 2 draw/func_800155A0.s:32, Spyro 3 draw/func_8001E460.s:30).
 * A taken call empties the captured set, retiring the previous epoch's set
 * first when it is the epoch's first call (cut_held_sets_capture).
 * When the routine appended anything, its primitives are captured, paired
 * and built in float under the tick's view (glint_build_exact), and under
 * extended LOD each far record's star is appended (glint_star_far); in a view
 * wider than 4:3 the stars its lateral test dropped are appended
 * (glint_reveal), and under extended LOD so are the records its box and far
 * culls dropped. The set is fed natively with the canonical trace when
 * the scene is live and RP_SPARKLE is selected; a non-empty set from a call
 * that appended nothing marks RP_SPARKLE drawn (render_path_mark_drawn),
 * since only a polybuf interval or the mark makes the sub-tick re-extract
 * feed the set. The census runs on every taken call.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the bracket declines and the ordinary dispatch must run the
 *         routine, else 1. */
static int on_glint_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    cut_held_sets_capture(&s_sets);
    s_item_n = 0; s_cap_n = 0; s_stars = 0; s_crosses = 0; s_pred_near = 0; s_pred_far = 0;
    s_hq_far = 0;
    s_wide_admit = 0; s_wide_cull = 0; s_range_admit = 0; s_range_cull = 0;
    const uint32_t star_before = s_fed_star_tris, cross_before = s_fed_cross_tris;
    /* A view wider than 4:3 or extended LOD can show a star the routine
     * emitted nothing for. */
    if (endp > startp || openpete_native_widescreen_factor() > 1.0f || openpete_no_lod) {
        ngte_view view;
        ngte_view_build(cpu, &view);
        glint_owner_scan(cpu);
        if (endp > startp) {
            glint_capture_prims(cpu, startp, endp);
            s_cap_n = s_item_n;
            glint_pair_depths(cpu, &view);
            glint_build_exact(cpu, &view);
            glint_vtx_cal();
            glint_star_far(cpu, &view);
        }
        glint_reveal(cpu, &view);
        if (s_item_n > 0) {
            if (feed && render_path_enabled(RP_SPARKLE)) {
                render_material_level_push(cpu);
                glint_feed_native();
                glint_trace("canon", 1.0);
                glint_vtx_present(0);
            }
            /* A set of stars the routine emitted nothing for records no
             * polybuf interval; the mark keeps the sub-tick re-extract
             * feeding it. */
            if (endp <= startp) render_path_mark_drawn(RP_SPARKLE);
        }
    }
    glint_census(cpu, startp, endp, star_before, cross_before);
    return hook_bracket_end(&k_bracket);
}

/* Sub-tick re-extract; documented at the declaration in glint.h. */
void glint_reextract(CPUState* cpu, int bracketed, double alpha, int hold) OP_REQUIRES(sim) {
    const int pick = cut_held_sets_enter(&s_sets, hold);
    if (hook_reextract_ready_held(RP_SPARKLE, pick)) {
        if (pick >= 0) {
            /* The staged camera is tick N-1's, the camera this set was drawn
             * under: the stars stand where that tick drew them. */
            for (int i = 0; i < s_item_n; i++) s_reb[i].active = 0;
        } else if (bracketed) {
            glint_rebase_all(cpu, alpha);
        }
        render_material_level_push(cpu);
        glint_feed_native();
        glint_trace("inter", bracketed ? alpha : 1.0);
        glint_vtx_present(bracketed && pick < 0 ? 1 : 0);
    }
    cut_held_sets_leave(&s_sets, pick);
}

/* The hook address is the layout's glint draw site, bound at boot: a static
 * initializer cannot read another TU's const. A game with no located routine
 * leaves it 0 and the registry dispatches nothing. */
static feature_dispatch_entry_t glint_dispatch[] = {
    { 0u, on_glint_draw, 0u },
    { 0u, NULL, 0u },
};

/** @brief Bind the glint dispatch entry to the layout's routine address.
 *
 * A game whose layout locates no routine leaves the address 0.
 * @param opts Unused. */
static void glint_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    glint_dispatch[0].addr = game_layout.glint.hook;
}

const feature_module_t glint_module = {
    .name           = "glint_native",
    .apply_options  = glint_apply_options,
    .dispatch_table = glint_dispatch,
};
