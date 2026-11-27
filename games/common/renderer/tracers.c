#include "openpete_thread_roles.h"
// games/common/renderer/tracers.c - the tracer ribbon feeders: two handwritten
// routines that build textured gouraud quad strips and link them into the 3D
// ordering table themselves, so none of their primitives reaches the 2D
// chokepoint the sprite feeder hooks. Both are the same routine in each game
// (ribbon: Spyro 2 func_8004B72C / mobyfunc.s:2936-3157, Spyro 3 func_8002FB68
// / mobyfunc.s:2940-3181, 217 of 222 instructions; mesh: Spyro 2 func_8004BAA4
// / mobyfunc.s:3180-3402, Spyro 3 func_8002FEE0 / mobyfunc.s:3182-3418, the
// same arithmetic under a different load-delay schedule). Every per-game
// address, count, stride and bound comes from game_layout.tracers; the walks
// below are one body.
//
// Two producers, one capture:
//
//   * the point-ring ribbon (asm/mobyfunc.s:2940-3181, Spyro 3 offsets). Walks
//     a 0x20-slot array of records (Spyro 3: 0x6C bytes; Spyro 2: 0x30 bytes),
//     each holding a circular buffer of packed unsigned XYZ halves (Spyro 3:
//     16 entries; Spyro 2: 6) plus a head index, a point count, a signed width
//     byte and a colour-plus-command word (Spyro 3: at +0x64 and
//     +0x68). It RTPS-projects every live point of a record into the
//     scratchpad, then walks consecutive pairs and appends one 13-word
//     POLY_GT4 per segment: the two projected
//     points each expanded by the record's half-width, which tapers linearly to
//     zero along the ribbon, and DPCS-faded from the record's colour. The width
//     rides in the high half of a packed screen coordinate while it is positive
//     and in the low half while it is negative, so the ribbon expands vertically
//     or horizontally by the sign of that byte.
//   * the indexed mesh (asm/mobyfunc.s:3182-3418). Takes one of eight 0x14-byte
//     descriptors: a vertex list of world XYZ triples ended by -1, a colour
//     table, an index list of 8-byte entries ended by -1, a pair of texture
//     words, and a right-shift applied to every camera-relative coordinate. It
//     projects the vertex list into the scratchpad as {screen XY, MAC3 and
//     outcode} pairs and appends one POLY_GT4 per index entry. It also stores
//     the highest ordering-table bucket it touched, so running its body twice
//     would move a value the next draw reads: the shared bracket runs the guest
//     draw exactly once.
//
// Every corner is built in float from the producer's own record, the camera
// and the routine's width law, at capture and again at each sub-tick present;
// the routine's primitives supply the pairing, the texture words and the
// colours, never a position (an integer re-run of each routine, the
// OPENPETE_TRACER_VTX probe, reproduces their corners). Depth is not carried
// by the primitives either - the PSX orders them by ordering-table bucket - so
// each one's view-Z is recovered from the producer's own record:
//
//   * the mesh leg replays the routine's own emit test out of the scratchpad
//     vertex table the routine just wrote, which still holds this call's
//     vertices: the pairing is then the routine's own arithmetic on its own
//     values, and the depth is the MAC3 average the routine bucketed by.
//   * the ribbon leg re-walks the record array and re-runs the routine's per
//     record tests (the camera-relative box test, then each point's RTPS in
//     the GTE's integer arithmetic for the shared-outcode, far and key tests)
//     to predict the segments each record emitted. When the total equals the
//     number of primitives captured they pair in emission order; otherwise
//     each primitive pairs with the prediction whose two points it lies on,
//     and one that none explains takes the mean depth of the nearest
//     prediction's record and keeps its captured corners.
//   * a mesh primitive past the replay's last paired entry takes the mean
//     depth of the call's paired ones.
//
// Both routines append each primitive at the tail of its bucket (ribbon
// mobyfunc.s:3151-3165, mesh :3380-3385), after the moby faces the moby draw
// spliced there. A paired item carries the bucket the replay computes, and an
// additive one contests the actor key image by it as the glow fan does: it
// loses a pixel to a moby face keyed into a nearer bucket, which the face's
// depth offset can put in front of a ribbon nearer in true depth. Against
// terrain the item keeps its true depth.
//
// Spyro 1's tracer.c is not made a client of this: its beam is re-derived in
// float from tracer-point gamestate with its own ribbon-width and colour laws,
// and these routines' records, width law and scratchpad tables differ from it.
// What the two share - the hook bracket, the polybuf walk,
// the primitive decode and the projection helpers - is already shared and is
// reused here unchanged.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the polybuf cursor
// and the ordering-table top; this feeder only reads them afterwards.

#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "renderer/cut_guard.h"     /* cut_held_sets_t: the held cut's tick N-1 set */
#include "renderer/orchestrator.h"  /* the canonical-render epoch the vertex lists carry */
#include "renderer/tracers.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_interp_objs.h"   /* the per-record prev/curr snapshots */
#include "openpete_psx.h"           /* openpete_psx_to_host_span */
#include "game_layout.h"
#include "renderer/subtick_anchor_chk.h"

static int s_sac_live;   /* the sub-tick anchor probe judges this present's points */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

#define TR (game_layout.tracers)

/* Record shape of the point-ring ribbon (asm/mobyfunc.s:2940-2947, 3096-3100
 * in the Spyro 3 image): the live pointer at +0x00, zero = free slot. */
#define RIB_LIVE         0x00u
#define RIB_POINT_MAX    256

/* Descriptor shape of the indexed mesh (asm/mobyfunc.s:3217-3218, 3277-3279). */
#define MSH_VERTS        0x00u
#define MSH_INDICES      0x08u
#define MSH_SHIFT        0x10u

/* The scratchpad table both routines project into holds 8 bytes per vertex:
 * the screen XY word and the MAC3-plus-outcode word (:3196-3200, :3218-3221). */
#define SPAD_MAX_VERTS   128

#define TR_MAX_PRIMS     512    /* primitives one call may contribute */
#define TR_MAX_TICK      2048   /* primitives one tick may contribute */
#define TR_MAX_LISTS     8      /* mesh vertex lists one tick may draw */

/* Where one corner of a primitive came from: the point the routine projected
 * to place it. `key` names the point's previous state in the keyed store - the
 * ribbon record, or the mesh vertex's own address - and is 0 on a corner with
 * no source. `age` is the ribbon point's place counted back from the record's
 * head. `w` is the point in full world units as the tick drew it, and (sx, sy)
 * and m3 its screen position and view-Z under the tick's camera. */
typedef struct {
    uint32_t key;
    int      age;
    int32_t  w[3];
    double   sx, sy, m3;
    int      ok;       /* the point stood in front of the near floor */
} tr_corner;

enum { TR_SRC_NONE = 0, TR_SRC_RIBBON, TR_SRC_MESH };

/* A clipped mesh quad keeps at most eight corners: each triangle of the
 * GPU's split keeps at most four. */
#define TR_POLY_MAX 8

/* One item's float build for one view: screen corners with their texture
 * coordinates, colours and colour residuals (ngte_cvert.rgbq, nonzero only on
 * a corner a cut placed). An unclipped build or a ribbon segment (fan 0)
 * holds four or three corners in the primitive's own order and feeds in the
 * GPU's quad split. A mesh primitive the near plane cut (fan 1) holds the
 * clipped triangles of that split, corners [0, n0) the first and [n0, n) the
 * second, each in winding order and fed as a fan from its first corner; a
 * part below 3 corners is empty. n below 3 feeds nothing. `near` marks a
 * build with a corner short of TR_NEAR_M3, which takes the near-plane path,
 * and `clipped` one the plane cut, whichever form it took. `vz` is the mean
 * full-world view Z of the shown points the corners sit about, under the
 * build's view. */
typedef struct {
    int      n, n0, fan, near, clipped;
    double   vz;
    float    x[TR_POLY_MAX], y[TR_POLY_MAX], u[TR_POLY_MAX], v[TR_POLY_MAX];
    uint32_t rgb[TR_POLY_MAX], rgbq[TR_POLY_MAX];
} tr_poly;

typedef struct {
    ngte_prim p;
    float     depth;
    int       key;             /* the ordering-table bucket the routine links it at, -1 unknown */
    int       paired;
    int       src;             /* TR_SRC_*: which producer's point each corner holds */
    uint32_t  ident;           /* OPENPETE_TAG_TRACER_RIBBON or _MESH: the producer */
    tr_corner c[4];
    tr_corner head;            /* ribbon: the record's head point, age 0 */
    int       wb;              /* ribbon: the record's signed width byte */
    int       count;           /* ribbon: the ring's point count */
    int       shift;           /* mesh: the descriptor's right shift */
    int       built;           /* b holds the float build */
    tr_poly   b;               /* the build under the tick's view */
    tr_poly   pp;              /* the build for the sub-tick present */
} tr_item;

static tr_item  s_item[TR_MAX_PRIMS];
static int      s_item_n;

/* The tick's whole set, replayed by the sub-tick leg: nine calls contribute to
 * one canonical present and the polybuf is reused before the next one. The
 * set the last capture of a new canonical epoch retired is kept beside it,
 * which a held camera cut shows when it is tick N-1's. */
static tr_item  s_tick_buf[2][TR_MAX_TICK];
static tr_item* s_tick = s_tick_buf[0];
static int      s_tick_n;
static tr_item* s_tick_old = s_tick_buf[1];
static int      s_tick_old_n;

/** @brief Exchange the tick's held set with the retired one, with their counts. */
static void tr_sets_swap(void) {
    tr_item* t = s_tick; s_tick = s_tick_old; s_tick_old = t;
    const int n = s_tick_n; s_tick_n = s_tick_old_n; s_tick_old_n = n;
}

static cut_held_sets_t s_sets = { tr_sets_swap, &s_tick_n, "tracers", { 0u, 0u }, 0u };

/* The mesh vertex lists the most recent VBL with a mesh call read: address
 * and vertex count, and the canonical-render epoch of that render. The set
 * is reported only while that epoch is the in-flight or the last closed one,
 * so a render that calls no mesh retires it. */
static uint32_t s_list_addr[TR_MAX_LISTS];
static uint32_t s_list_n[TR_MAX_LISTS];
static int      s_lists;
static unsigned s_list_frame = 0xFFFFFFFFu;
static uint32_t s_list_epoch;

static uint32_t s_code_hist[256];
static uint32_t s_tag_hist[256];
static uint32_t s_fed_tris;
static int      s_last_predicted;   /* segments the record walk predicted */
static int      s_live;             /* records holding a live pointer */
static int      s_boxed;            /* of those, records the box test dropped */
static int32_t  s_cam_half[3];      /* the camera position >> 1 the ribbon routine read */

/** @brief Stores one decoded primitive as an unpaired item in the call's item set.
 *
 * The item starts at depth 1 with no source, no float build and zeroed
 * corners.
 * @param ctx Unused.
 * @param s The primitive.
 * @return 1 when stored, 0 when the set holds TR_MAX_PRIMS items. */
static int tr_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= TR_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 1.0f;   /* unpaired: the near guard (view-Z 1) */
    s_item[s_item_n].key = -1;
    s_item[s_item_n].paired = 0;
    s_item[s_item_n].src = TR_SRC_NONE;
    s_item[s_item_n].built = 0;
    memset(s_item[s_item_n].c, 0, sizeof s_item[s_item_n].c);
    memset(&s_item[s_item_n].head, 0, sizeof s_item[s_item_n].head);
    s_item_n++;
    return 1;
}

/** @brief Decodes the primitives a routine call appended to the polybuf into the call's item set.
 *
 * Both routines append one 13-word primitive at a time (tag length 12), so
 * the walk admits tags up to 12 words and at most TR_MAX_PRIMS primitives.
 * Each primitive becomes an unpaired item at the near-guard depth; the code
 * and tag histograms count every packet walked.
 * @param cpu The guest CPU state.
 * @param start The polybuf cursor before the call.
 * @param end The polybuf cursor after the call. */
static void tr_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 12, .raw_words_max = 13, .guard = TR_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, tr_prim_sink, NULL);
}

/* ---- corner sources --------------------------------------------------------
 *
 * The float build needs, per corner, the point the routine placed it at. Both
 * routines load the camera-relative offset (x - cx, cy - y, cz - z) as
 * (VZ0, VX0, VY0) (ribbon :2974-2999, mesh :3223-3237); a uniform scale of that
 * vector, the ribbon's halving or the mesh's right shift, leaves the
 * perspective divide's quotient unchanged, so both project here in full world
 * units through one function. */

/* The view-Z below which a point has no stable screen position: the ribbon
 * drops a segment whose two ends average under 128 in its halved units
 * (:3152-3154), which is 256 in full world units. */
#define TR_NEAR_M3 256.0

/** @brief Projects a world point to the screen under a view, in full world units.
 *
 * The camera-relative offset is (x - cx, cy - y, cz - z), the vector both
 * routines load as (VZ0, VX0, VY0).
 * @param v The view.
 * @param p The world point.
 * @param o Receives the projection. */
static void tr_project_world(const ngte_view* v, const double p[3], ngte_proj* o) {
    const double vo[3] = { p[0] - v->cam[0], v->cam[1] - p[1], v->cam[2] - p[2] };
    ngte_project_offset(v, vo, o);
}

/** @brief Fills a corner's source record and its screen position and view-Z under the tick's view.
 *
 * Sets ok when the point's view-Z is at or past TR_NEAR_M3.
 * @param c The corner, written.
 * @param v The tick's view.
 * @param key The previous-state key: the ribbon record or the vertex address.
 * @param age The ribbon point's place counted back from the head; 0 for a mesh
 *        vertex.
 * @param w The point in full world units. */
static void tr_corner_set(tr_corner* c, const ngte_view* v, uint32_t key, int age,
                          const int32_t w[3]) {
    c->key = key;
    c->age = age;
    memcpy(c->w, w, sizeof c->w);
    const double p[3] = { (double)w[0], (double)w[1], (double)w[2] };
    ngte_proj o;
    tr_project_world(v, p, &o);
    c->sx = o.sx;
    c->sy = o.sy;
    c->m3 = o.m3;
    c->ok = o.m3 >= TR_NEAR_M3;
}

/* ---- ribbon leg: predict the segments the record walk emitted -------------- */

/* One projected ring point: the routine's outcode word and SZ3 from the
 * integer re-run of its RTPS, its integer screen point, and its float MAC3
 * and source corner under the tick's view. */
typedef struct { int32_t oc; int32_t sz3; int isx, isy; double m3; tr_corner c; } rib_pt;

/* One predicted segment: its depth and the two points it joins, the head-side
 * one first. The routine writes corners 0 and 2 about the head-side point and
 * corners 1 and 3 about the next one (:3125-3141). `head`, `wb` and `count`
 * are the record's head point, signed width byte and ring point count, which
 * set every point's half-width. */
typedef struct {
    float     depth;
    int       key;              /* the bucket the routine links it at (:3151-3153) */
    tr_corner a, b, head;
    int       wb, count;
    int       ax, ay, bx, by;   /* the two points' integer screen positions (SXY2) */
    float     rec_depth;        /* the mean depth of its record's segments */
} rib_seg;

/** @brief Computes the ribbon routine's outcode word for a screen position (mobyfunc.s:2488-2508).
 *
 * The packed screen coordinate (y in the high half) is shifted left five
 * and gains one low bit per screen edge the point falls outside: 1 for a
 * packed word at or below zero, 2 for y at or past 240, 4 for x at or below
 * zero, 8 for x at or past 512. Only the low four bits are tested, and only
 * as an AND across a record's points or across a segment's two ends.
 * @param sx The screen x.
 * @param sy The screen y.
 * @return The outcode word. */
static int32_t rib_outcode(int sx, int sy) {
    const int32_t xy = (int32_t)(((uint32_t)(sy & 0xFFFF) << 16) | ((uint32_t)sx & 0xFFFFu));
    int32_t oc = (int32_t)((uint32_t)xy << 5);
    int32_t t = xy;
    if (!(t > 0)) oc += 1;
    t = xy - 0x00F00000;
    if (!(t < 0)) oc += 2;
    t = (int32_t)((uint32_t)xy << 16);
    if (!(t > 0)) oc += 4;
    t = t - 0x02000000;
    if (!(t < 0)) oc += 8;
    return oc;
}

/** @brief Re-runs the routine's RTPS of one ring point in the GTE's integer arithmetic and takes its outcode (:2974-3010).
 *
 * The routine packs VXY0 as (camY - Y) + ((camZ - Z) << 16) with one add, so
 * a negative low half borrows one from VY0, and loads X - camX as VZ0; it
 * runs RTPS under the camera rotation with the zero translation, OFX, OFY
 * and H the GTE holds (H_canon, never the --fov scaled H), and keeps SZ3 and
 * the outcode of SXY2 (rib_outcode). These are the values the routine's
 * record, segment and key tests read.
 * @param v The tick's view: the matrix, the offsets and H_canon.
 * @param vx X - camX in half world units, the routine's VZ0.
 * @param vy camY - Y, the routine's VX0.
 * @param vz camZ - Z, the routine's VY0.
 * @param pt Receives sz3, oc, isx and isy. */
static void rib_rtps(const ngte_view* v, int32_t vx, int32_t vy, int32_t vz, rib_pt* pt) {
    static const int k_tr0[3] = { 0, 0, 0 };
    const uint32_t vxy = (uint32_t)vy + ((uint32_t)vz << 16);
    ngte_rtps_out o;
    ngte_rtps_int(v->cam_M, k_tr0, (int16_t)(vxy & 0xFFFFu), (int16_t)(vxy >> 16),
                  (int16_t)vx, v->ofx, v->ofy, (int)v->H_canon, &o);
    pt->sz3 = o.sz3;
    pt->isx = o.sx;
    pt->isy = o.sy;
    pt->oc  = rib_outcode(o.sx, o.sy);
}

/** @brief Decodes the point a given number of places back from the head of a ribbon record's ring.
 *
 * The record image is guest RAM or a keyed-store blob. The packed word at
 * TR.packed_off holds the head index (byte 0) and the point count (byte 1);
 * the point's three unsigned halves are shifted left one as the routine
 * loads them, which gives half world units. An age past the tail is
 * clamped to the tail.
 * @param rec The record image.
 * @param len The image's length in bytes.
 * @param age Places back from the head; 0 is the head.
 * @param hw Receives the point in half world units.
 * @return The ring's point count, or 0 when the image is too short or the
 *         packed word holds no walkable ring (count below 2 or above
 *         TR.ring_n, or head not below count). */
static int rib_ring_point(const uint8_t* rec, uint32_t len, int age, int32_t hw[3]) {
    if (len < TR.packed_off + 4u) return 0;
    uint32_t packed;
    memcpy(&packed, rec + TR.packed_off, 4);
    const int head  = (int)(packed & 0xFFu);
    const int count = (int)((packed >> 8) & 0xFFu);
    if (count < 2 || count > TR.ring_n || head >= count) return 0;
    if (age >= count) age = count - 1;
    const int idx = ((head - age) % count + count) % count;
    const uint32_t off = TR.points_off + (uint32_t)idx * 6u;
    if (off + 6u > len) return 0;
    uint16_t h[3];
    memcpy(h, rec + off, 6);
    for (int k = 0; k < 3; k++) hw[k] = (int32_t)((uint32_t)h[k] << 1);
    return count;
}

/** @brief Projects one ribbon record's points head first, mirroring the routine's per-record rejects.
 *
 * The rejects are any point outside the camera-relative box (:2408-2437,
 * counted in s_boxed), every point off one common screen edge (:2496) and
 * the head's SZ3 at or past TR.far_sz (:2540). Each point's outcode, SZ3,
 * MAC3 and source corner (keyed by the record, aged by its place from the
 * head) are written to pt.
 * @param cpu Unused.
 * @param v The tick's view.
 * @param rec The record's guest address.
 * @param cam The camera position shifted right one, as the routine reads it.
 * @param pt Receives the projected points, head first.
 * @return The point count, or 0 when the routine drops the record. */
static int rib_project_record(CPUState* cpu, const ngte_view* v, uint32_t rec,
                              const int32_t cam[3], rib_pt* pt) {
    (void)cpu;
    const uint8_t* host = (const uint8_t*)openpete_psx_to_host_span(rec, TR.stride);
    if (!host) return 0;
    int32_t hw[3];
    const int count = rib_ring_point(host, TR.stride, 0, hw);
    if (count == 0) return 0;
    int32_t and_oc = -1;
    for (int k = 0; k < count; k++) {
        rib_ring_point(host, TR.stride, k, hw);
        const int32_t vx = hw[0] - cam[0];   /* the routine's IR3 */
        const int32_t vy = cam[1] - hw[1];   /* IR1 */
        const int32_t vz = cam[2] - hw[2];   /* IR2 */
        if (vx <= -TR.box || vx >= TR.box || vy <= -TR.box || vy >= TR.box ||
            vz <= -TR.box || vz >= TR.box) { s_boxed++; return 0; }
        ngte_proj o;
        ngte_project_gte(v->cam_M, NULL, (double)vy, (double)vz, (double)vx,
                         v->ofx, v->ofy, v, 0, &o);
        pt[k].m3 = o.m3;
        rib_rtps(v, vx, vy, vz, &pt[k]);
        and_oc &= pt[k].oc;
        const int32_t w[3] = { hw[0] * 2, hw[1] * 2, hw[2] * 2 };
        tr_corner_set(&pt[k].c, v, rec, k, w);
    }
    if ((and_oc & 0xF) > 0) return 0;
    if (pt[0].sz3 - TR.far_sz >= 0) return 0;
    return count;
}

/** @brief Predicts the segments one live ribbon record emitted, in emission order, and gives them the record's mean depth.
 *
 * The points are projected by rib_project_record; a consecutive pair is
 * dropped when its two outcodes share an edge bit (:2652) or its
 * ordering-table key is negative (:2692). Each kept segment carries its
 * view-Z from the pair's mean MAC3, its two source corners head side first
 * and their integer screen points.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param rec The record's guest address.
 * @param out Receives the segments from index n.
 * @param n The segments predicted so far.
 * @param cap The capacity of out; segments past it are counted, not written.
 * @return The count of segments predicted so far, this record's included. */
static int rib_predict_record(CPUState* cpu, const ngte_view* v, uint32_t rec,
                              rib_seg* out, int n, int cap) {
    static rib_pt pt[RIB_POINT_MAX];
    const int count = rib_project_record(cpu, v, rec, s_cam_half, pt);
    /* :3079-3080 the width is the signed byte 2 of the packed word. */
    const int wb = (int)(int8_t)(uint8_t)(cpu->read_word(rec + TR.packed_off) >> 16);
    const int first = n;
    double sum = 0.0;
    for (int k = 0; k + 1 < count; k++) {
        if ((pt[k].oc & pt[k + 1].oc & 0xF) > 0) continue;        /* :2652 */
        const int32_t key = ((pt[k].sz3 + pt[k + 1].sz3) >> 7) - 2;
        if (key < 0) continue;                                    /* :2692 */
        /* The record's halves are quarter world units and the routine halves
         * the whole vector, so its MAC3 is half the world value. */
        const float d = openpete_native_scene_viewz(
            openpete_worldvz_scaled((float)((pt[k].m3 + pt[k + 1].m3) * 0.5), 2.0f));
        sum += d;
        if (n < cap) {
            out[n] = (rib_seg){ .depth = d, .key = key, .a = pt[k].c, .b = pt[k + 1].c, .head = pt[0].c,
                                .wb = wb, .count = count, .ax = pt[k].isx, .ay = pt[k].isy,
                                .bx = pt[k + 1].isx, .by = pt[k + 1].isy };
        }
        n++;
    }
    for (int i = first; i < n && i < cap; i++) out[i].rec_depth = (float)(sum / (n - first));
    return n;
}

/** @brief Re-walks the ribbon record array and predicts every segment the routine emitted, in emission order.
 *
 * Every live record (counted in s_live) goes through rib_predict_record in
 * slot order, the routine's order.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param out Receives the segments.
 * @param cap The capacity of out; segments past it are counted, not written.
 * @return The count of predicted segments. */
static int rib_predict(CPUState* cpu, const ngte_view* v, rib_seg* out, int cap) {
    int n = 0;
    s_live = 0; s_boxed = 0;
    /* The camera position the routine reads, shifted right one like its own
     * `srl` so the box test compares the same values. */
    for (int k = 0; k < 3; k++)
        s_cam_half[k] = (int32_t)(cpu->read_word(game_layout.cam.position + 4u * (uint32_t)k) >> 1);
    for (uint32_t s = 0; s < TR.slots; s++) {
        const uint32_t rec = TR.ring + s * TR.stride;
        if (cpu->read_word(rec + RIB_LIVE) == 0) continue;
        s_live++;
        n = rib_predict_record(cpu, v, rec, out, n, cap);
    }
    return n;
}

/** @brief Binds one captured ribbon primitive to a predicted segment: its depth, the ribbon source and its corners.
 *
 * Corners 0 and 2 come from the head-side point and 1 and 3 from the next
 * (:3125-3141); the record's head point, width byte and point count set
 * every point's half-width.
 * @param it The primitive.
 * @param g The segment. */
static void rib_bind(tr_item* it, const rib_seg* g) {
    it->depth = g->depth;
    it->key = g->key;
    it->paired = 1;
    it->src = TR_SRC_RIBBON;
    it->c[0] = it->c[2] = g->a;
    it->c[1] = it->c[3] = g->b;
    it->head = g->head;
    it->wb = g->wb;
    it->count = g->count;
}

/* The squared screen distance, in PSX pixels summed over a segment's two
 * ends, inside which a captured primitive is the segment's own: the
 * routine's corners are its integer points plus and minus the half-width,
 * so the mid of corners 0 and 2 and of 1 and 3 land on them within a pixel
 * of the packed-add rounding. */
#define RIB_PAIR_TOL2 8.0

/** @brief The squared distance between a captured primitive's two corner-pair mids and a segment's two integer points.
 * @param it The primitive.
 * @param g The segment.
 * @return The summed squared distance in PSX pixels. */
static double rib_seg_dist2(const tr_item* it, const rib_seg* g) {
    const double ax = 0.5 * ((double)it->p.x[0] + it->p.x[2]), ay = 0.5 * ((double)it->p.y[0] + it->p.y[2]);
    const double bx = 0.5 * ((double)it->p.x[1] + it->p.x[3]), by = 0.5 * ((double)it->p.y[1] + it->p.y[3]);
    return (ax - g->ax) * (ax - g->ax) + (ay - g->ay) * (ay - g->ay) +
           (bx - g->bx) * (bx - g->bx) + (by - g->by) * (by - g->by);
}

static unsigned long s_rib_order, s_rib_matched, s_rib_meaned, s_rib_depth1;

/** @brief Pairs a call whose predicted count differs from the captured one, primitive by primitive.
 *
 * The routine emits each record's segments in ring order and the records in
 * slot order, so a cursor walks the predictions with the primitives: a
 * primitive pairs with the first prediction at or past the cursor whose two
 * points it lies on (RIB_PAIR_TOL2), and the cursor moves past it. A
 * primitive no prediction explains takes the mean depth of the record whose
 * prediction lies nearest, and stays unpaired: it keeps its captured corners.
 * @param pred The predictions.
 * @param n Their count, at most TR_MAX_PRIMS. */
static void rib_pair_walk(const rib_seg* pred, int n) {
    int cur = 0;
    for (int i = 0; i < s_item_n; i++) {
        tr_item* it = &s_item[i];
        int best = -1, hit = -1;
        double bd = INFINITY;
        for (int j = 0; j < n; j++) {
            const double d = rib_seg_dist2(it, &pred[j]);
            if (j >= cur && hit < 0 && d <= RIB_PAIR_TOL2) hit = j;
            if (d < bd) { bd = d; best = j; }
        }
        if (hit >= 0) { rib_bind(it, &pred[hit]); cur = hit + 1; s_rib_matched++; continue; }
        if (best >= 0) { it->depth = pred[best].rec_depth; s_rib_meaned++; }
        else s_rib_depth1++;
    }
}

/** @brief Pairs the captured ribbon primitives with the record walk's predicted segments.
 *
 * Runs rib_predict and records its count in s_last_predicted. When the count
 * equals the captured primitive count, the primitives bind in emission order
 * (rib_bind); otherwise rib_pair_walk pairs them one by one. A call with no
 * prediction leaves its primitives at the near-guard depth.
 * @param cpu The guest CPU state.
 * @param v The tick's view. */
static void rib_pair_depths(CPUState* cpu, const ngte_view* v) {
    static rib_seg pred[TR_MAX_PRIMS];
    const int n = rib_predict(cpu, v, pred, TR_MAX_PRIMS);
    s_last_predicted = n;
    if (n == s_item_n && n <= TR_MAX_PRIMS) {
        for (int i = 0; i < n; i++) rib_bind(&s_item[i], &pred[i]);
        s_rib_order += (unsigned long)n;
        return;
    }
    rib_pair_walk(pred, n < TR_MAX_PRIMS ? n : TR_MAX_PRIMS);
}

/* ---- mesh leg: replay the emit test out of the routine's own scratchpad ---- */

/** @brief Records a mesh vertex list the draw read, for the per-object capture.
 *
 * The list ends at the routine's -1 terminator (:3223-3225), bounded by the
 * scratchpad table's SPAD_MAX_VERTS. The first note on a new VBL empties
 * the set; a repeated address, an address outside KSEG0 RAM and a list past
 * TR_MAX_LISTS are ignored. Each note stamps the set with the in-flight
 * canonical-render epoch. The capture runs on every VBL and the draw once
 * per tick, so the set is replaced by the next VBL that draws and retired by
 * a render that calls no mesh (tracers_mesh_lists), not consumed by the
 * capture: a vertex missing from one capture is evicted from the store and
 * pairs as a spawn.
 * @param cpu The guest CPU state.
 * @param vlist The vertex list's guest address. */
static void msh_note_list(CPUState* cpu, uint32_t vlist) OP_REQUIRES(sim) {
    if (s_list_frame != openpete_frame) { s_list_frame = openpete_frame; s_lists = 0; }
    s_list_epoch = spyro_native_canon_epoch_stamp();
    if (vlist < 0x80000000u || vlist >= 0x80800000u) return;
    for (int i = 0; i < s_lists; i++) if (s_list_addr[i] == vlist) return;
    if (s_lists >= TR_MAX_LISTS) return;
    uint32_t n = 0;
    while (n < SPAD_MAX_VERTS && cpu->read_word(vlist + n * 12u) != 0xFFFFFFFFu) n++;
    s_list_addr[s_lists] = vlist;
    s_list_n[s_lists] = n;
    s_lists++;
}

/** @brief Records the vertex list a mesh descriptor names when it is a KSEG0 RAM address (msh_note_list).
 *
 * @param cpu The guest CPU state.
 * @param desc The mesh descriptor's guest address.
 * @return 1 when the list was a KSEG0 RAM address, else 0. */
static int msh_note_desc(CPUState* cpu, uint32_t desc) OP_REQUIRES(sim) {
    const uint32_t vlist = cpu->read_word(desc + MSH_VERTS);
    if (vlist < 0x80000000u || vlist >= 0x80800000u) return 0;
    msh_note_list(cpu, vlist);
    return 1;
}

uint32_t tracers_mesh_vertex_key(uint32_t va) {
    return TRACERS_MESH_KEY_TAG | (va & TRACERS_MESH_KEY_MASK);
}

int tracers_mesh_lists(uint32_t* addr, uint32_t* nverts, int max) OP_REQUIRES(sim) {
    if (s_list_epoch != spyro_native_canon_epoch_stamp() &&
        !spyro_native_canon_epoch_fresh(s_list_epoch)) return 0;
    int n = 0;
    for (int i = 0; i < s_lists && n < max; i++, n++) {
        addr[n] = s_list_addr[i];
        nverts[n] = s_list_n[i];
    }
    return n;
}

/** @brief Sets a mesh item's four source corners from the vertices its index entry names.
 *
 * Corner k is the entry's index byte k (:3358-3378 store them at +0x08,
 * +0x14, +0x20, +0x2C); the vertex is the 12-byte world XYZ triple at that
 * index of the descriptor's vertex list, keyed by tracers_mesh_vertex_key.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param it The item.
 * @param vlist The vertex list's guest address.
 * @param off The four scratchpad offsets (index * 8) of the entry.
 * @param shift The descriptor's right shift, kept for the probe's integer re-run. */
static void msh_set_corners(CPUState* cpu, const ngte_view* v, tr_item* it,
                            uint32_t vlist, const uint32_t off[4], int shift) {
    it->src = TR_SRC_MESH;
    it->shift = shift;
    for (int k = 0; k < 4; k++) {
        const uint32_t va = vlist + (off[k] >> 3) * 12u;
        const int32_t w[3] = {
            (int32_t)cpu->read_word(va + 0u),
            (int32_t)cpu->read_word(va + 4u),
            (int32_t)cpu->read_word(va + 8u),
        };
        tr_corner_set(&it->c[k], v, tracers_mesh_vertex_key(va), 0, w);
    }
}

/** @brief Whether every vertex index of a mesh index entry lies inside the scratchpad table.
 * @param w0 The entry's first word: four 8-bit vertex indices (:3236-3251).
 * @return 1 when all four 8-byte slots lie inside SPAD_MAX_VERTS, else 0. */
static int msh_entry_in_table(uint32_t w0) {
    for (int k = 0; k < 4; k++)
        if (((w0 >> (8 * k)) & 0xFFu) >= SPAD_MAX_VERTS) return 0;
    return 1;
}

static unsigned long s_msh_order, s_msh_meaned, s_msh_depth1;

/** @brief Replays the routine's projection of one mesh vertex into the word it stores at the vertex's scratchpad slot + 4.
 *
 * The vertex's offset from the camera, shifted right by the descriptor's
 * shift (`srav`), loads VX0 = camera Y - Y, VY0 = camera Z - Z, VZ0 = X -
 * camera X and is RTPSed under the camera matrix with a zero translation
 * (Spyro 3 mobyfunc.s:3227-3243). The word is MAC3 << 16 or'd with the
 * routine's edge bits, computed from SXY2 as the routine branches
 * (:3244-3263), delay slots included: +1 when SXY2 - 0xD0000 is not above 0;
 * the comparand is then SXY2 << 16 when SXY2 is below 0xE40000, else the
 * bits + 2 (the routine writes the sum to the comparand, not to the bits);
 * +4 when the comparand is not above 0, +8 when the comparand - 0x2000000 is
 * not below 0. The subtractions wrap as 32-bit words.
 * @param v The tick's view.
 * @param cam The camera position the routine reads.
 * @param w The vertex's world XYZ.
 * @param sh The descriptor's shift, low five bits.
 * @return The replayed slot word. */
static int32_t msh_vertex_word(const ngte_view* v, const int32_t cam[3], const int32_t w[3], int sh) {
    static const int k_tr0[3] = { 0, 0, 0 };
    ngte_rtps_out o;
    ngte_rtps_int(v->cam_M, k_tr0, (int16_t)((cam[1] - w[1]) >> sh), (int16_t)((cam[2] - w[2]) >> sh),
                  (int16_t)((w[0] - cam[0]) >> sh), v->ofx, v->ofy, (int)v->H_canon, &o);
    const int32_t sxy = (int32_t)(((uint32_t)(uint16_t)o.sy << 16) | (uint16_t)o.sx);
    int32_t bits = 0;
    if (!((int32_t)((uint32_t)sxy - 0xD0000u) > 0)) bits += 1;
    const int32_t t = (int32_t)((uint32_t)sxy - 0xE40000u) < 0 ? (int32_t)((uint32_t)sxy << 16) : bits + 2;
    if (!(t > 0)) bits += 4;
    if (!((int32_t)((uint32_t)t - 0x2000000u) < 0)) bits += 8;
    return (int32_t)(((uint32_t)o.mac[2] << 16) | (uint32_t)bits);
}

/** @brief The ordering-table bucket the routine links a mesh entry at, replayed from the vertex list and the camera.
 *
 * The four vertices' replayed MAC3 halves (msh_vertex_word) are summed,
 * shifted right 9 and left by the descriptor's shift (:3336-3341). Reads the
 * vertex list, the camera and the descriptor only, never the routine's
 * scratchpad table.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param vlist The vertex list's guest address.
 * @param off The four slot offsets (index * 8) of the entry.
 * @param shift The descriptor's shift.
 * @param words Receives the four replayed slot words.
 * @return The bucket, or -1 when the replay puts it outside 0..0x7FF. */
static int msh_replay_key(CPUState* cpu, const ngte_view* v, uint32_t vlist, const uint32_t off[4],
                          int shift, int32_t words[4]) {
    int32_t cam[3];
    for (int k = 0; k < 3; k++) cam[k] = (int32_t)cpu->read_word(game_layout.cam.position + 4u * (uint32_t)k);
    int32_t zsum = 0;
    for (int k = 0; k < 4; k++) {
        const uint32_t va = vlist + (off[k] >> 3) * 12u;
        const int32_t w[3] = { (int32_t)cpu->read_word(va), (int32_t)cpu->read_word(va + 4u),
                               (int32_t)cpu->read_word(va + 8u) };
        words[k] = msh_vertex_word(v, cam, w, shift & 31);
        zsum += words[k] >> 16;
    }
    const int32_t key = (int32_t)((uint32_t)(zsum >> 9) << (unsigned)(shift & 31));
    return (key < 0 || key - 0x7FF > 0) ? -1 : key;
}

/* OPENPETE_TRACER_MESH_KEYCHK: entries the routine emitted, compared against the replay. */
static unsigned long s_mk_entries OP_GUARDED_BY(sim), s_mk_match OP_GUARDED_BY(sim), s_mk_miss OP_GUARDED_BY(sim),
                     s_mk_words OP_GUARDED_BY(sim), s_mk_wmatch OP_GUARDED_BY(sim);   /* thread-roles: the mesh hook, on sim */

/** @brief Whether OPENPETE_TRACER_MESH_KEYCHK asks for the replay check, latched on first use.
 * @return 1 when set to a value not starting with '0', else 0. */
static int msh_keychk_on(void) OP_REQUIRES(sim) {
    static int v OP_GUARDED_BY(sim) = -1;   /* thread-roles: the mesh hook, on sim */
    if (v < 0) { const char* e = getenv("OPENPETE_TRACER_MESH_KEYCHK"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief Prints the replayed-bucket check's running totals to stderr, one line per mesh call. */
static void msh_keychk_line(void) OP_REQUIRES_SHARED(sim) {
    fprintf(stderr, "[tracer-meshkey f=%u] entries=%lu key_match=%lu key_mismatch=%lu slot_words=%lu word_match=%lu\n",
            openpete_frame, s_mk_entries, s_mk_match, s_mk_miss, s_mk_words, s_mk_wmatch);
}

/** @brief Instrument: compares an emitted entry's replayed bucket and slot words with the routine's own scratchpad table.
 * @param cpu The guest CPU state.
 * @param off The four slot offsets of the entry.
 * @param words The replayed slot words.
 * @param key The replayed bucket.
 * @param guest_key The bucket the routine computed from its table. */
static void msh_keychk(CPUState* cpu, const uint32_t off[4], const int32_t words[4], int key, int guest_key) OP_REQUIRES(sim) {
    s_mk_entries++;
    if (key == guest_key) s_mk_match++;
    else {
        s_mk_miss++;
        fprintf(stderr, "[tracer-meshkey f=%u] key replay=%d routine=%d\n", openpete_frame, key, guest_key);
    }
    for (int k = 0; k < 4; k++) {
        s_mk_words++;
        if ((int32_t)cpu->read_word(TR.spad + off[k] + 4u) == words[k]) s_mk_wmatch++;
    }
}

/** @brief Gives the mesh primitives the replay could not reach the mean depth of those it paired.
 *
 * The replay stops at the index list's terminator, or at an entry naming a
 * slot outside the scratchpad table, before the captured primitives run out
 * only when it and the routine disagree; such a primitive stays unpaired and
 * keeps its captured corners. A call that paired none leaves them at the
 * near-guard depth.
 * @param paired The count of leading items the replay paired. */
static void msh_mean_rest(int paired) {
    s_msh_order += (unsigned long)paired;
    if (paired >= s_item_n) return;
    double sum = 0.0;
    for (int i = 0; i < paired; i++) sum += s_item[i].depth;
    for (int i = paired; i < s_item_n; i++) {
        if (paired > 0) { s_item[i].depth = (float)(sum / paired); s_msh_meaned++; }
        else s_msh_depth1++;
    }
}

/** @brief Pairs the captured mesh primitives with the entries the routine emitted, in order.
 *
 * Replays the routine's emit test over the descriptor's index list against
 * the scratchpad table it left: an entry is skipped on a common outcode edge
 * or a key outside 0..0x7FF, otherwise the next item takes the depth from
 * the entry's summed SZ and the descriptor's shift and is marked paired. When
 * the vertex list is a valid KSEG0 RAM address the list is noted and each
 * paired item gets its four vertex corners and the bucket the routine links
 * it at, replayed from the vertex list and the camera (msh_replay_key); an
 * item without a vertex list keeps bucket -1. The walk ends at the -1
 * terminator, at an entry with an index outside the scratchpad table, or
 * when every item is paired; msh_mean_rest then places the items left. An
 * index list outside KSEG0 RAM leaves every item unpaired at the near-guard
 * depth.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param desc The mesh descriptor's guest address. */
static void msh_pair_depths(CPUState* cpu, const ngte_view* v, uint32_t desc) OP_REQUIRES(sim) {
    const uint32_t ilist = cpu->read_word(desc + MSH_INDICES);
    if (ilist < 0x80000000u || ilist >= 0x80800000u) { s_msh_depth1 += (unsigned long)s_item_n; return; }
    const uint32_t vlist = cpu->read_word(desc + MSH_VERTS);
    const int have_verts = msh_note_desc(cpu, desc);
    const int shift = (int)cpu->read_word(desc + MSH_SHIFT);
    const float mul = (shift >= 0 && shift < 24) ? (float)(1u << (unsigned)shift) : 1.0f;

    int i = 0;
    for (uint32_t e = ilist; i < s_item_n; e += 8u) {
        const uint32_t w0 = cpu->read_word(e);
        if (w0 == 0xFFFFFFFFu || !msh_entry_in_table(w0)) break;
        /* :3236-3251 four 8-bit vertex indices, each scaled to its 8-byte
         * scratchpad slot before the table read. */
        const uint32_t off[4] = {
            (w0 & 0xFFu) << 3,
            ((uint32_t)((int32_t)w0 >> 5)) & 0x7F8u,
            ((uint32_t)((int32_t)w0 >> 13)) & 0x7F8u,
            ((uint32_t)((int32_t)w0 >> 21)) & 0x7F8u,
        };
        int32_t and_oc = -1, zsum = 0;
        for (int k = 0; k < 4; k++) {
            const int32_t w1 = (int32_t)cpu->read_word(TR.spad + off[k] + 4u);
            and_oc &= w1;
            zsum += w1 >> 16;                            /* :3241 sra 16 */
        }
        if ((and_oc & 0xF) > 0) continue;                /* :3256 */
        int32_t key = zsum >> 9;
        key = (int32_t)((uint32_t)key << (unsigned)(shift & 31));
        if (key < 0 || key - 0x7FF > 0) continue;        /* :3262-3264 */
        s_item[i].depth = openpete_native_scene_viewz(
            openpete_worldvz_scaled((float)zsum * 0.25f, mul));
        if (have_verts) {
            int32_t words[4];
            s_item[i].key = msh_replay_key(cpu, v, vlist, off, shift, words);
            if (msh_keychk_on()) msh_keychk(cpu, off, words, s_item[i].key, key);
        }
        s_item[i].paired = 1;
        if (have_verts) msh_set_corners(cpu, v, &s_item[i], vlist, off, shift);
        i++;
    }
    msh_mean_rest(i);
    if (msh_keychk_on()) msh_keychk_line();
}

/* ---- feed ---------------------------------------------------------------- */

/** @brief Reports whether the tracer feed is enabled; OPENPETE_TRACERS=0 disables it.
 *
 * Only this feed is suppressed; the guest draws still run, so an A/B
 * measures the ribbons' painted pixels from one binary. Latched on the
 * first call.
 * @return 0 when OPENPETE_TRACERS starts with '0', else 1. */
static int tr_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRACERS"); v = !(e && *e && *e == '0'); }
    return v;
}

/* Whether the held set carries a sub-tick build (px, py). Clear on every
 * canonical feed and at the end of the tick's interval, where the replay emits
 * the capture's build verbatim. */
static int s_reb_active;

/* The near census (OPENPETE_TRACER_NEAR=LO-HI names the VBLs printed): per
 * producer, the items one present feeds, those with no source (unpaired),
 * those the float build left to the routine's corners (unbuilt), those with
 * a corner short of TR_NEAR_M3 (near), those the near plane cut (clipped) and, of those, the ones it removed whole (culled).
 * Run totals print at exit, split canonical and sub-tick. */
typedef struct {
    unsigned long items, unpaired, unbuilt, near, clipped, culled;
} tr_near_n;
static tr_near_n     s_near_tot[2][2];   /* [0 canonical, 1 sub-tick][ident] */
static unsigned long s_near_presents[2];

/** @brief Prints the near census's run totals to stderr, registered with atexit by tr_near_census. */
static void tr_near_report(void) {
    OP_ROLE_ASSERT(sim);
    static const char* const leg[2] = { "canon", "inter" };
    static const char* const src[2] = { "ribbon", "mesh" };
    for (int l = 0; l < 2; l++)
        for (int k = 0; k < 2; k++) {
            const tr_near_n* t = &s_near_tot[l][k];
            fprintf(stderr, "[tracer-near total %s %s] presents=%lu items=%lu unpaired=%lu "
                    "unbuilt=%lu near=%lu clipped=%lu culled=%lu\n",
                    leg[l], src[k], s_near_presents[l], t->items, t->unpaired, t->unbuilt,
                    t->near, t->clipped, t->culled);
        }
}

/** @brief Counts one fed set into the near census and prints its line when the VBL is in OPENPETE_TRACER_NEAR's range.
 *
 * @param set The items fed.
 * @param n The item count. */
static void tr_near_census(const tr_item* set, int n) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    static int reg;
    const int print = env_frame_range_active(&s_range, "OPENPETE_TRACER_NEAR", openpete_frame);
    if (s_range.lo < 0) return;
    if (!reg) { reg = 1; atexit(tr_near_report); }
    const int l = s_reb_active ? 1 : 0;
    tr_near_n c[2];
    memset(c, 0, sizeof c);
    for (int i = 0; i < n; i++) {
        tr_near_n* t = &c[set[i].ident == OPENPETE_TAG_TRACER_MESH ? 1 : 0];
        t->items++;
        if (set[i].src == TR_SRC_NONE) { t->unpaired++; continue; }
        if (!set[i].built) { t->unbuilt++; continue; }
        const tr_poly* q = l ? &set[i].pp : &set[i].b;
        if (q->near) t->near++;
        if (q->clipped) t->clipped++;
        if (q->n < 3) t->culled++;
    }
    s_near_presents[l]++;
    for (int k = 0; k < 2; k++) {
        tr_near_n* t = &s_near_tot[l][k];
        t->items += c[k].items; t->unpaired += c[k].unpaired; t->unbuilt += c[k].unbuilt;
        t->near += c[k].near; t->clipped += c[k].clipped; t->culled += c[k].culled;
    }
    if (print)
        fprintf(stderr, "[tracer-near f=%u %s] ribbon items=%lu unpaired=%lu unbuilt=%lu "
                "near=%lu clipped=%lu culled=%lu mesh items=%lu unpaired=%lu unbuilt=%lu "
                "near=%lu clipped=%lu culled=%lu\n",
                openpete_frame, l ? "inter" : "canon",
                c[0].items, c[0].unpaired, c[0].unbuilt, c[0].near, c[0].clipped, c[0].culled,
                c[1].items, c[1].unpaired, c[1].unbuilt, c[1].near, c[1].clipped, c[1].culled);
}

/** @brief The depth an item feeds at: the tick's, moved by its shown points' view-Z change on a sub-tick build.
 *
 * The tick's depth is the routine's own (a ribbon segment's mean MAC3, a
 * mesh entry's summed SZ), from the same points the corners sit about, so
 * the sub-tick build moves it by the mean view Z the points gained or lost
 * between the tick's view and the present's. Equal to the tick's depth for
 * the capture's build, and for an unbuilt item, which has no shown points.
 * @param it The item.
 * @param q The build being fed, or NULL for an unbuilt item.
 * @return The scene depth, clamped at 1. */
static float tr_item_depth(const tr_item* it, const tr_poly* q) {
    if (q != &it->pp || sac_tick_depth(1)) return it->depth;
    return openpete_native_scene_viewz(openpete_worldvz_full((float)((double)it->depth + q->vz - it->b.vz)));
}

/** @brief Feeds a fan of a clipped build's corners from its first corner to the native scene.
 *
 * @param q The build.
 * @param b The fan's first corner.
 * @param e One past its last corner; below b + 3 feeds nothing.
 * @param depth The scene depth.
 * @param tpp The texture page, or NULL for untextured.
 * @param blend The native blend code.
 * @param key The bucket the routine links the item at, or -1.
 * @return The number of triangles fed. */
static int tr_feed_fan(const tr_poly* q, int b, int e, float depth, const ngte_tpage* tpp, int blend,
                       int key) OP_REQUIRES_SHARED(sim) {
    int fed = 0;
    for (int i = b + 1; i + 1 < e; i++) {
        const int idx[4] = { b, i, i + 1, i + 1 };
        float x[4], y[4], u[4], v[4];
        uint32_t rgb[4], rgbq[4];
        for (int k = 0; k < 4; k++) {
            const int j = idx[k];
            x[k] = q->x[j]; y[k] = q->y[j]; u[k] = q->u[j]; v[k] = q->v[j];
            rgb[k] = q->rgb[j]; rgbq[k] = q->rgbq[j];
        }
        fed += ngte_feed_flat_quad_q(x, y, u, v, rgb, rgbq, depth, tpp, blend, 3, key);
    }
    return fed;
}

/** @brief Feeds one item to the native scene as world triangles.
 *
 * An unclipped build or an unbuilt item feeds as the primitive does: a quad
 * split into (0,1,2) and (1,3,2), a triangle (0,1,2) alone. A mesh build the
 * near plane cut feeds each clipped triangle of that split as a fan
 * (tr_feed_fan). Every vertex takes the
 * item's single depth (tr_item_depth). A built item's corners, texture coordinates and
 * colours are its float build, the sub-tick one while the rebase is active
 * and the capture's otherwise; an unbuilt item feeds the routine's corners.
 * While the key pass runs (openpete_native_key_resolve_on) each triangle
 * carries the bucket the routine linked the item at (ngte_feed_flat_quad_q),
 * so an additive item loses a pixel to a moby face keyed into a nearer
 * bucket and keeps true depth against terrain. Only 3 and 4 vertex
 * primitives are fed. Counts the triangles fed.
 * @param it The item. */
static void tr_add_prim(const tr_item* it) OP_REQUIRES_SHARED(sim) {
    const ngte_prim* p = &it->p;
    if (p->nv != 3 && p->nv != 4) return;
    ngte_tpage tp;
    const int blend = ngte_prim_tpage_blend(p, &tp);
    const int key = openpete_native_key_resolve_on() ? it->key : -1;
    const ngte_tpage* tpp = p->textured ? &tp : NULL;
    float x[4], y[4], u[4], v[4];
    uint32_t rgb[4], rgbq[4];
    const tr_poly* q = it->built ? (s_reb_active ? &it->pp : &it->b) : NULL;
    const float depth = tr_item_depth(it, q);
    if (q && q->fan) {
        s_fed_tris += (uint32_t)(tr_feed_fan(q, 0, q->n0, depth, tpp, blend, key) +
                                 tr_feed_fan(q, q->n0, q->n, depth, tpp, blend, key));
        return;
    }
    if (q && q->n < 3) return;
    const int last = p->nv - 1;           /* a triangle fills the unread corner 3 */
    const int idx[4] = { 0, 1, 2, last };
    for (int k = 0; k < 4; k++) {
        const int j = idx[k];
        if (q) { x[k] = q->x[j]; y[k] = q->y[j]; u[k] = q->u[j]; v[k] = q->v[j]; rgb[k] = q->rgb[j]; rgbq[k] = q->rgbq[j]; }
        else   { x[k] = p->x[j]; y[k] = p->y[j]; u[k] = (float)p->u[j]; v[k] = (float)p->v[j]; rgb[k] = p->rgb[j]; rgbq[k] = 0u; }
    }
    s_fed_tris += (uint32_t)ngte_feed_flat_quad_q(x, y, u, v, rgb, rgbq, depth, tpp, blend, p->nv, key);
}

/** @brief Feeds a set of items to the native scene under the tracer tag.
 *
 * Each item is tagged with its producer's identity (ribbon or mesh). Does
 * nothing when OPENPETE_TRACERS disables the feed or the set is empty.
 * @param set The items.
 * @param n The item count. */
static void tr_feed_native(const tr_item* set, int n) OP_REQUIRES(sim) {
    if (!tr_feed_enabled()) return;
    if (n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    tr_near_census(set, n);
    for (int i = 0; i < n; i++) {
        openpete_native_scene_set_tag(OPENPETE_TAG_TRACER(set[i].ident, 0));
        tr_add_prim(&set[i]);
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Stamps every item of the call's capture with its producer's identity.
 *
 * @param ident OPENPETE_TAG_TRACER_RIBBON or OPENPETE_TAG_TRACER_MESH. */
static void tr_set_ident(uint32_t ident) {
    for (int i = 0; i < s_item_n; i++) s_item[i].ident = ident;
}

static void tr_build_call(CPUState* cpu, const ngte_view* v);
static void tr_vtx_tick_close(void);
static unsigned s_epoch;        /* canonical epochs captured */

/** @brief Builds the call's items in float and appends them to the tick's held set for the sub-tick replay.
 *
 * The first call of a canonical epoch retires the previous epoch's set
 * (cut_held_sets_capture), empties the held one, since several calls
 * contribute to one canonical present, and opens a new epoch for the vertex
 * probe. The items are then built under the tick's view (tr_build_call);
 * items past TR_MAX_TICK are dropped. Clears the rebuild so a canonical feed
 * emits the capture's build.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @return The index of the call's first item in the held set. */
static int tr_hold(CPUState* cpu, const ngte_view* v) OP_REQUIRES(sim) {
    if (cut_held_sets_capture(&s_sets)) {
        s_tick_n = 0;
        s_epoch++;
        tr_vtx_tick_close();
    }
    tr_build_call(cpu, v);
    const int first = s_tick_n;
    for (int i = 0; i < s_item_n && s_tick_n < TR_MAX_TICK; i++) s_tick[s_tick_n++] = s_item[i];
    s_reb_active = 0;
    return first;
}

/** @brief Prints one stderr line per hook call inside the VBL range OPENPETE_TRACER_CENSUS names (LO-HI or one VBL).
 *
 * The line carries the polybuf interval, the primitives captured by GPU
 * code, the native triangles fed from them, the segments the record walk
 * predicted, the unpaired primitives, the live and boxed record counts,
 * the held set's size, and the run's totals of ribbon primitives bound in
 * emission order, matched one by one, given their record's mean depth or
 * left at depth 1, and of mesh primitives paired, given the call's mean depth
 * or left at depth 1. A capture whose fed count is zero measured nothing.
 * @param leg The producer's name, "ribbon" or "mesh".
 * @param start The polybuf cursor before the call.
 * @param end The polybuf cursor after the call.
 * @param fed_before s_fed_tris before the call. */
static void tr_census(const char* leg, uint32_t start, uint32_t end, uint32_t fed_before) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_TRACER_CENSUS", openpete_frame)) return;
    int unpaired = 0;
    for (int i = 0; i < s_item_n; i++) if (!s_item[i].paired) unpaired++;
    fprintf(stderr, "[tracer-census f=%u %s] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, leg, start, end, end - start, s_item_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_item_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    fprintf(stderr, " fed_tris=%u predicted=%d unpaired=%d live=%d boxed=%d tick=%d"
                    " total rib order=%lu matched=%lu meaned=%lu depth1=%lu"
                    " msh order=%lu meaned=%lu depth1=%lu\n",
            s_fed_tris - fed_before, s_last_predicted, unpaired, s_live, s_boxed, s_tick_n,
            s_rib_order, s_rib_matched, s_rib_meaned, s_rib_depth1,
            s_msh_order, s_msh_meaned, s_msh_depth1);
}

/* ---- hooks ---------------------------------------------------------------- */

static hook_bracket_state s_hb_rib;
static const hook_bracket_desc k_rib = {
    .path = RP_TRACER, .prof_tag = "tracer3.srend", .st = &s_hb_rib,
};
static hook_bracket_state s_hb_msh;
/* The mesh leg's descriptor argument, read before the guest draw runs: the
 * routine spills and reuses every saved register, so a0 is gone afterwards. */
static uint32_t s_msh_desc;
/** @brief Hook-bracket pre step of the mesh leg: saves the descriptor argument in a0.
 *
 * The routine reuses a0, so the value is read before the guest draw runs.
 * @param cpu The guest CPU state.
 * @param feed Unused.
 * @param verify Unused. */
static void msh_pre(CPUState* cpu, int feed, int verify) {
    OP_ROLE_ASSERT(sim);
    (void)feed; (void)verify;
    s_msh_desc = cpu->a0;
}
static const hook_bracket_desc k_msh = {
    .path = RP_TRACER, .prof_tag = "tracer3.srend", .pre = msh_pre, .st = &s_hb_msh,
};

/** @brief Dispatch hook of the ribbon routine: brackets the guest draw and feeds its primitives.
 *
 * After the draw, decodes the appended primitives, pairs them with the
 * predicted segments, holds them for the sub-tick replay and, when the
 * bracket asks for a feed and the tracer path is enabled, feeds them to the
 * native scene. Reads guest RAM only.
 * @param cpu The guest CPU state.
 * @param addr The routine's guest address.
 * @return 0 when the bracket declines the call, else the bracket's end
 *         result. */
static int on_ribbon(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_rib, &startp, &endp, &feed, &verify))
        return 0;
    s_last_predicted = 0;
    s_item_n = 0;
    const uint32_t fed_before = s_fed_tris;
    {
        if (endp > startp) tr_capture_prims(cpu, startp, endp);
        tr_set_ident(OPENPETE_TAG_TRACER_RIBBON);
        ngte_view view;
        ngte_view_build(cpu, &view);
        rib_pair_depths(cpu, &view);
        const int first = tr_hold(cpu, &view);
        render_material_level_push(cpu);
        if (feed && render_path_enabled(RP_TRACER))
            tr_feed_native(&s_tick[first], s_tick_n - first);
    }
    tr_census("ribbon", startp, endp, fed_before);
    return hook_bracket_end(&k_rib);
}

/** @brief Dispatch hook of the mesh routine: brackets the guest draw and feeds its primitives.
 *
 * As on_ribbon, with the depths and corners paired from the descriptor read
 * before the draw (s_msh_desc); a call that appended no primitives does no
 * capture and only notes the vertex list the routine projected.
 * @param cpu The guest CPU state.
 * @param addr The routine's guest address.
 * @return 0 when the bracket declines the call, else the bracket's end
 *         result. */
static int on_mesh(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_msh, &startp, &endp, &feed, &verify))
        return 0;
    s_last_predicted = 0;
    s_item_n = 0;
    const uint32_t fed_before = s_fed_tris;
    if (endp > startp) {
        tr_capture_prims(cpu, startp, endp);
        tr_set_ident(OPENPETE_TAG_TRACER_MESH);
        ngte_view view;
        ngte_view_build(cpu, &view);
        msh_pair_depths(cpu, &view, s_msh_desc);
        const int first = tr_hold(cpu, &view);
        render_material_level_push(cpu);
        if (feed && render_path_enabled(RP_TRACER))
            tr_feed_native(&s_tick[first], s_tick_n - first);
    } else {
        (void)msh_note_desc(cpu, s_msh_desc);   /* projected, none emitted: the list stays live */
    }
    tr_census("mesh", startp, endp, fed_before);
    return hook_bracket_end(&k_msh);
}

/* ---- float build ------------------------------------------------------------
 *
 * Each corner is built from the point the routine placed it about, projected
 * in float under the view being drawn, the ribbon's corners then offset by the
 * routine's width law. At capture the view is the tick's and the point the
 * tick's. On a sub-tick present the view is the staged camera's and the point
 * the lerp of the previous tick's point and the tick's, paired by the record's
 * slot and the point's place counted back from the ring's head, so at alpha 0
 * the ribbon lies along the previous tick's ring and the head sweeps to the
 * tick's head over the interval; a ring that grew this tick holds its new tail
 * on the previous tail. A mesh corner pairs its own vertex by address. A point
 * with no usable previous state holds the tick's world position, and the
 * camera's motion alone moves it. An item with a corner short of TR_NEAR_M3
 * is clipped against a near plane in camera space and projected in float,
 * at capture and at every present alike, so the shown pose decides what
 * remains of it. At the end of the tick's interval the replay emits the
 * capture's build verbatim. A built item's depth is the canonical pairing's,
 * moved by the view Z its shown points gained or lost between the tick's
 * view and the present's (tr_item_depth).
 *
 * Out of scope, its own channel of the same record: the ribbon's DPCS fade by
 * the head's depth and along the ring. */

/* A point's speed ceiling, in full world units per tick. The store is keyed by
 * slot address, so a slot a new record respawns into inside one tick pairs
 * across two different ribbons; the owner test catches a reuse by another
 * owner and this one a reuse by the same owner, at a distance no point covers
 * in a tick. */
#define TR_REUSE_CEILING 4096.0

static unsigned long s_reb_stat[3][2]; /* corners per TR_SRC_*: 0 lerped, 1 held */
static double        s_reb_worldmax;  /* the largest per-tick world step paired */

/** @brief Looks up a keyed-store record that holds a usable previous and current state.
 *
 * Refuses a record with no previous state, timestamps that are not
 * increasing, a blob shorter than need, or a pair whose prev was not
 * observed up to curr (cut_feeder_pair_is_tick).
 * @param key The record's key.
 * @param need The minimum blob length in bytes.
 * @return The record, or NULL when refused. */
static const openpete_interp_obj_t* tr_pair(uint32_t key, uint32_t need) {
    const openpete_interp_obj_t* o = openpete_interp_objs_find(key);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return NULL;
    if ((uint32_t)o->blob_len < need) return NULL;
    if (!cut_feeder_pair_is_tick("tracer", key, o, 0u, need)) return NULL;
    return o;
}

/** @brief Looks up a corner's point in the previous tick's keyed-store state, in full world units.
 *
 * A mesh corner reads its vertex's previous triple; a ribbon corner decodes
 * the point of the same age from the record's previous image.
 * @param src The corner's producer, TR_SRC_MESH or TR_SRC_RIBBON.
 * @param c The corner.
 * @param w Receives the previous point.
 * @return 1 when found; 0 for a record or vertex with no previous state, a
 *         stale pair (tr_pair), a ribbon slot whose
 *         live pointer changed, or a ring that does not decode. */
static int tr_prev_point(int src, const tr_corner* c, int32_t w[3]) {
    if (src == TR_SRC_MESH) {
        const openpete_interp_obj_t* o = tr_pair(c->key, 12u);
        if (!o) return 0;
        memcpy(w, o->prev, 12);
        return 1;
    }
    const openpete_interp_obj_t* o = tr_pair(c->key, TR.packed_off + 4u);
    if (!o || memcmp(o->prev + RIB_LIVE, o->curr + RIB_LIVE, 4) != 0) return 0;
    int32_t hw[3];
    if (!rib_ring_point(o->prev, o->blob_len, c->age, hw)) return 0;
    for (int k = 0; k < 3; k++) w[k] = hw[k] * 2;
    return 1;
}

/** @brief Resolves the world point a corner shows at a present below alpha 1.
 *
 * The point is the tick's point moved toward its previous point by
 * (1 - alpha); a previous point farther than TR_REUSE_CEILING is not used and
 * the point stays at the tick's. Counts the lerped and held points per
 * producer.
 * @param src The corner's producer.
 * @param c The corner.
 * @param alpha The present's position in the tick interval, below 1.
 * @param p Holds the tick's point on entry; receives the shown point.
 * @param prev Receives the previous point when one was used.
 * @return 1 when the point was lerped toward a previous point, 0 when held. */
static int tr_point_at(int src, const tr_corner* c, double alpha, double p[3], double prev[3]) {
    int32_t pw[3];
    int lerped = 0;
    if (tr_prev_point(src, c, pw)) {
        double d[3], sq = 0.0;
        for (int j = 0; j < 3; j++) { d[j] = (double)pw[j] - p[j]; sq += d[j] * d[j]; }
        if (sq <= TR_REUSE_CEILING * TR_REUSE_CEILING) {
            for (int j = 0; j < 3; j++) { prev[j] = pw[j]; p[j] += d[j] * (1.0 - alpha); }
            s_reb_worldmax = fmax(s_reb_worldmax, sqrt(sq));
            lerped = 1;
        }
    }
    s_reb_stat[src][lerped ? 0 : 1]++;
    return lerped;
}

/** @brief Prints why a held point found no previous state, at sub-tick anchor probe level 2.
 *
 * The reasons are the keyed store's: no entry, no previous snapshot, stamps
 * out of order, a stale pair (cut_pair_is_tick), or another
 * refusal (a ring that does not decode, a changed live pointer, a step past
 * TR_REUSE_CEILING).
 * @param src The corner's producer.
 * @param c The corner. */
static void tr_sac_held_line(int src, const tr_corner* c) {
    if (sac_level() < 2) return;
    const openpete_interp_obj_t* so = openpete_interp_objs_find(c->key);
    const char* why = "other";
    if (!so) why = "nokey";
    else if (!so->have_prev) why = "noprev";
    else if (so->curr_ns <= so->prev_ns) why = "order";
    else if (!cut_pair_is_tick(so)) why = "stale";
    const double T = (double)g_openpete_tick_period_ns;
    const double age = so ? (double)((int64_t)openpete_interp_present_now() - (int64_t)so->curr_ns) / T : -1.0;
    const double span = (so && so->have_prev) ? (double)(so->curr_ns - so->prev_ns) / T : -1.0;
    fprintf(stderr, "[sac-held %s vbl=%u] key=%08X why=%s curr_age_ticks=%.2f span_ticks=%.2f\n",
            src == TR_SRC_MESH ? "mesh" : "ribbon", openpete_frame, c->key, why, age, span);
}

/** @brief Hands one shown point to the sub-tick anchor probe with the keyed store's stamps.
 * @param src The corner's producer.
 * @param c The corner.
 * @param v The present's view.
 * @param prev The previous point the shown point was lerped from, or NULL when held.
 * @param o The shown point's projection, as fed. */
static void tr_sac_point(int src, const tr_corner* c, const ngte_view* v, const double* prev,
                         const ngte_proj* o) {
    if (o->m3 < TR_NEAR_M3) return;
    if (!prev) tr_sac_held_line(src, c);
    const openpete_interp_obj_t* so = openpete_interp_objs_find(c->key);
    const double cw[3] = { (double)c->w[0], (double)c->w[1], (double)c->w[2] };
    sac_judge(src == TR_SRC_MESH ? SAC_MESH : SAC_RIBBON, c->key ^ ((uint32_t)c->age << 24), prev, cw,
              so ? (int64_t)so->prev_ns : 0, so ? (int64_t)so->curr_ns : 0, o->sx, o->sy, v);
}

/** @brief Hands one built item's depth on this present to the sub-tick anchor probe.
 *
 * The true depth is the tick's moved by the shown points' view-Z change; the
 * stale one is the tick's.
 * @param it The item, its sub-tick build done.
 * @param alpha The present's alpha. */
static void tr_sac_depth(const tr_item* it, double alpha) {
    const double truth = (double)it->depth + it->pp.vz - it->b.vz;
    sac_depth(it->src == TR_SRC_MESH ? SAC_MESH : SAC_RIBBON, it->depth, tr_item_depth(it, &it->pp), truth, alpha);
}

/** @brief Projects the point a corner was placed about as a present shows it.
 *
 * Below alpha 1 the point is tr_point_at's; at alpha 1 or above it is the
 * tick's. The projection's m1, m2 and m3 are the point in camera space; its
 * screen position is valid only at m3 >= TR_NEAR_M3, which the caller tests.
 * While the sub-tick anchor probe is live each shown point is handed to it
 * (tr_sac_point).
 * @param src The corner's producer.
 * @param c The corner.
 * @param v The view to project under.
 * @param alpha The present's position in the tick interval; 1 or above shows the tick's point.
 * @param o Receives the projection.
 * @return 1 when the corner has a source, 0 otherwise. */
static int tr_shown(int src, const tr_corner* c, const ngte_view* v, double alpha, ngte_proj* o) {
    if (!c->key) return 0;
    double p[3] = { (double)c->w[0], (double)c->w[1], (double)c->w[2] };
    double prev[3];
    const int lerped = alpha < 1.0 && tr_point_at(src, c, alpha, p, prev);
    tr_project_world(v, p, o);
    if (s_sac_live && alpha < 1.0) tr_sac_point(src, c, v, lerped ? prev : NULL, o);
    return 1;
}

/** @brief The ribbon's half-width at its head in screen pixels, from the head's view-Z.
 *
 * The routine divides the record's signed width byte, shifted left 11, by the
 * head's SZ3 floored at 0x80, and turns a zero quotient into +1 or -1 by the
 * byte's sign (Spyro 3 mobyfunc.s:3078-3092). Its SZ3 is the view-Z of the
 * halved camera-relative vector (:2984-2999), half the full-unit m3. The
 * width scales with the view's --fov factor, as the projection does.
 * @param wb The signed width byte.
 * @param m3 The head's view-Z in full world units.
 * @param s The view's --fov scale.
 * @return The signed half-width; its sign picks the axis. */
static double rib_head_width(int wb, double m3, double s) {
    const double sz = fmax(m3 * 0.5, 128.0);
    double a = (double)wb * 2048.0 / sz;
    if (fabs(a) < 1.0) a = (wb >= 0) ? 1.0 : -1.0;
    return a * s;
}

/** @brief Places one ribbon corner about its point's screen position.
 *
 * The point's half-width tapers linearly from the head's to zero at the tail:
 * the routine adds 0x1000 / (count - 1) to a blend per point and subtracts
 * that fraction of the head's width (:3045-3049, :3128-3132). A non-negative
 * width offsets along screen Y and a negative one along screen X, the packed
 * word's high or low half (:3120-3122, :3132-3136); corners 0 and 1 sit at
 * the point minus the offset, 2 and 3 plus it (:3124-3127, :3138-3141).
 * @param sx The point's screen X.
 * @param sy The point's screen Y.
 * @param a The head's signed half-width (rib_head_width).
 * @param age The point's place counted back from the head; a point the near
 *        plane cut between two ring points carries the fraction between them.
 * @param count The ring's point count, at least 2.
 * @param k The corner index, 0..3.
 * @param x Receives the corner's screen X.
 * @param y Receives the corner's screen Y. */
static void rib_corner_xy(double sx, double sy, double a, double age, int count, int k,
                          float* x, float* y) {
    const double w = a * (1.0 - age / (double)(count - 1));
    const double o = (k < 2) ? -w : w;
    *x = (float)(w >= 0.0 ? sx : sx + o);
    *y = (float)(w >= 0.0 ? sy + o : sy);
}

/* The plane a cut item is clipped at, the terrain's eye plane (16 world
 * units). The cut is made once; a crossing that projects far off screen is
 * left to the rasterizer's clip. */
#define TR_EYE_M3          16.0

/** @brief Projects a camera-space point to the screen in float, with no GTE divide cap or saturation.
 *
 * Equal to the GTE-faithful projection wherever that one is uncapped and
 * unsaturated (m3 above half the GTE's H, |m1| and |m2| inside int16).
 * @param v The view.
 * @param m1 Camera-space X.
 * @param m2 Camera-space Y.
 * @param m3 Camera-space depth, positive.
 * @param x Receives the screen X.
 * @param y Receives the screen Y. */
static void tr_screen_f(const ngte_view* v, double m1, double m2, double m3, double* x, double* y) {
    const double q = v->H_eff / m3;
    *x = v->ofx + m1 * q;
    *y = v->ofy + m2 * q;
}

/** @brief The camera-space corner a clip interpolates, from a corner's shown point and the primitive's corner attributes.
 *
 * @param o The corner's shown projection (m1, m2, m3).
 * @param p The primitive.
 * @param k The corner index.
 * @return The clip corner. */
static ngte_cvert tr_cvert(const ngte_proj* o, const ngte_prim* p, int k) {
    ngte_cvert c;
    memset(&c, 0, sizeof c);
    c.m1 = o->m1; c.m2 = o->m2; c.m3 = o->m3;
    c.rgb = p->rgb[k];
    c.u = (float)p->u[k]; c.v = (float)p->v[k];
    return c;
}

/** @brief Projects one camera-space corner in float into a build's slot with its texture coordinates and colour.
 *
 * @param v The view.
 * @param c The corner, at or past TR_EYE_M3.
 * @param o The build.
 * @param j The slot. */
static void tr_poly_put(const ngte_view* v, const ngte_cvert* c, tr_poly* o, int j) {
    double x, y;
    tr_screen_f(v, c->m1, c->m2, c->m3, &x, &y);
    o->x[j] = (float)x; o->y[j] = (float)y;
    o->u[j] = c->u; o->v[j] = c->v; o->rgb[j] = c->rgb; o->rgbq[j] = c->rgbq;
}

/** @brief Clips a mesh primitive in camera space against the near plane, triangle by triangle of the GPU's split, and projects what remains in float.
 *
 * Each triangle of the split the unclipped feed draws, (0,1,2) and for a
 * quad (1,3,2), is cut once at TR_EYE_M3 (ngte_near_clip_tri); a crossing
 * takes ngte_clip_lerp of its edge: camera-space position, texture
 * coordinates, and the colour with its residual. When no corner is cut the
 * build keeps the primitive's corner order and feeds in the quad split, or
 * is empty when every corner lies behind the plane; otherwise each
 * triangle's output is one fan part, and a triangle wholly behind the plane
 * contributes none.
 * @param v The view.
 * @param in The corners in the primitive's order.
 * @param n The corner count, 3 or 4.
 * @param o Receives the build. */
static void msh_clip(const ngte_view* v, const ngte_cvert in[4], int n, tr_poly* o) {
    static const int split[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
    const int ntri = n == 4 ? 2 : 1;
    ngte_clip_ref ref[2][4];
    int cnt[2] = { 0, 0 }, cut = 0;
    for (int s = 0; s < ntri; s++) {
        const double z[3] = { in[split[s][0]].m3, in[split[s][1]].m3, in[split[s][2]].m3 };
        int c = 0;
        cnt[s] = ngte_near_clip_tri(z, TR_EYE_M3, ref[s], &c);
        cut |= c;
    }
    o->near = 1;
    o->clipped = cut;
    o->fan = cut;
    if (!cut) {
        o->n = o->n0 = cnt[0] ? n : 0;
        for (int k = 0; k < n; k++) tr_poly_put(v, &in[k], o, k);
        return;
    }
    o->n = 0;
    for (int s = 0; s < ntri; s++) {
        for (int k = 0; k < cnt[s]; k++) {
            const ngte_clip_ref* r = &ref[s][k];
            const ngte_cvert* a = &in[split[s][r->a]];
            const ngte_cvert c = r->cross ? ngte_clip_lerp(*a, in[split[s][r->b]], TR_EYE_M3) : *a;
            tr_poly_put(v, &c, o, o->n++);
        }
        if (s == 0) o->n0 = o->n;
    }
}

/** @brief Fills an unclipped build's texture coordinates and colours from the primitive.
 *
 * @param p The primitive.
 * @param o The build, its n set. */
static void tr_poly_attrs(const ngte_prim* p, tr_poly* o) {
    o->fan = 0;
    o->near = 0;
    o->clipped = 0;
    o->n0 = o->n;
    for (int k = 0; k < o->n; k++) {
        o->u[k] = (float)p->u[k]; o->v[k] = (float)p->v[k]; o->rgb[k] = p->rgb[k]; o->rgbq[k] = 0u;
    }
}

/** @brief Projects an item's corners as a present shows them and reports whether all stand in front of the near floor.
 *
 * @param it The item.
 * @param v The view.
 * @param alpha The present's position in the tick interval.
 * @param n The corner count.
 * @param s Receives the n shown projections.
 * @param o Receives in vz the mean view Z of the n shown points.
 * @return -1 when a corner has no source, 1 when every shown corner is at or
 *         past TR_NEAR_M3, else 0. */
static int tr_shown_corners(const tr_item* it, const ngte_view* v, double alpha, int n,
                            ngte_proj s[4], tr_poly* o) {
    int front = 1;
    double sum = 0.0;
    for (int k = 0; k < n; k++) {
        if (!tr_shown(it->src, &it->c[k], v, alpha, &s[k])) return -1;
        if (s[k].m3 < TR_NEAR_M3) front = 0;
        sum += s[k].m3;
    }
    o->vz = sum / n;
    return front;
}

/** @brief Builds a mesh item: each corner its vertex's projection, the polygon clipped at the near plane when a corner falls short of TR_NEAR_M3.
 *
 * @param it The item.
 * @param v The view.
 * @param alpha The present's position in the tick interval.
 * @param o Receives the build.
 * @return 1 when built, 0 when a corner has no source. */
static int msh_build(const tr_item* it, const ngte_view* v, double alpha, tr_poly* o) {
    const ngte_prim* p = &it->p;
    const int n = p->nv == 3 ? 3 : 4;
    ngte_proj s[4];
    const int front = tr_shown_corners(it, v, alpha, n, s, o);
    if (front < 0) return 0;
    if (front) {
        o->n = n;
        for (int k = 0; k < n; k++) { o->x[k] = (float)s[k].sx; o->y[k] = (float)s[k].sy; }
        tr_poly_attrs(p, o);
        return 1;
    }
    ngte_cvert in[4];
    for (int k = 0; k < n; k++) in[k] = tr_cvert(&s[k], p, k);
    msh_clip(v, in, n, o);
    return 1;
}

/** @brief Moves one end of a ribbon segment the near plane cut onto the plane, toward the other end.
 *
 * The end's two corners take the crossing of their edges to the other end's
 * corners (ngte_clip_lerp: camera-space position, texture coordinates and
 * colour), and its ring place the same fraction toward the other end's.
 * @param e The cut end's corners (0 and 2, or 1 and 3), rewritten.
 * @param f The other end's corners, in the same order.
 * @param age The cut end's ring place, rewritten.
 * @param fage The other end's ring place.
 * @param zc The plane's depth. */
static void rib_cut_end(ngte_cvert e[2], const ngte_cvert f[2], double* age, double fage, double zc) {
    const double t = (zc - e[0].m3) / (f[0].m3 - e[0].m3);
    *age += t * (fage - *age);
    e[0] = ngte_clip_lerp(e[0], f[0], zc);
    e[1] = ngte_clip_lerp(e[1], f[1], zc);
}

/** @brief Clips a ribbon segment against the near plane in camera space and builds its quad.
 *
 * End A holds corners 0 and 2, end B corners 1 and 3. A segment whose ends
 * both stand behind TR_EYE_M3 builds nothing; an end behind it moves onto it
 * (rib_cut_end), cut once. Each corner is then projected in float and offset
 * by the width law at its end's ring place, and the build stays a quad in
 * corner order.
 * @param it The item.
 * @param v The view.
 * @param a The head's signed half-width.
 * @param s The four corners' shown projections.
 * @param o Receives the build. */
static void rib_clip(const tr_item* it, const ngte_view* v, double a, const ngte_proj s[4],
                     tr_poly* o) {
    const ngte_prim* p = &it->p;
    const ngte_cvert A0[2] = { tr_cvert(&s[0], p, 0), tr_cvert(&s[2], p, 2) };
    const ngte_cvert B0[2] = { tr_cvert(&s[1], p, 1), tr_cvert(&s[3], p, 3) };
    const double ga0 = (double)it->c[0].age, gb0 = (double)it->c[1].age;
    o->fan = 0;
    o->near = 1;
    ngte_cvert A[2] = { A0[0], A0[1] }, B[2] = { B0[0], B0[1] };
    double ga = ga0, gb = gb0;
    const int ain = A[0].m3 >= TR_EYE_M3, bin = B[0].m3 >= TR_EYE_M3;
    o->clipped = !ain || !bin;
    if (!ain && !bin) { o->n = o->n0 = 0; return; }
    if (!ain) rib_cut_end(A, B0, &ga, gb0, TR_EYE_M3);
    if (!bin) rib_cut_end(B, A0, &gb, ga0, TR_EYE_M3);
    const ngte_cvert* c[4] = { &A[0], &B[0], &A[1], &B[1] };
    const double g[4] = { ga, gb, ga, gb };
    o->n = o->n0 = 4;
    for (int k = 0; k < 4; k++) {
        double x, y;
        tr_screen_f(v, c[k]->m1, c[k]->m2, c[k]->m3, &x, &y);
        rib_corner_xy(x, y, a, g[k], it->count, k, &o->x[k], &o->y[k]);
        o->u[k] = c[k]->u; o->v[k] = c[k]->v; o->rgb[k] = c[k]->rgb; o->rgbq[k] = c[k]->rgbq;
    }
}

/** @brief Builds a ribbon item: each corner its point's projection offset by the width law, the segment clipped at the near plane when an end falls short of TR_NEAR_M3.
 *
 * The half-width is the law's at the head's shown view-Z, which the law
 * floors at 0x80 halved units (rib_head_width); a head with no source uses
 * the tick's. A segment reaching in front of TR_NEAR_M3 is built by rib_clip.
 * @param it The item.
 * @param v The view.
 * @param alpha The present's position in the tick interval.
 * @param o Receives the build.
 * @return 1 when built, 0 when a corner has no source. */
static int rib_build(const tr_item* it, const ngte_view* v, double alpha, tr_poly* o) {
    ngte_proj h;
    const double hm3 = tr_shown(it->src, &it->head, v, alpha, &h) ? h.m3 : it->head.m3;
    const double a = rib_head_width(it->wb, hm3, v->s);
    ngte_proj s[4];
    const int front = tr_shown_corners(it, v, alpha, 4, s, o);
    if (front < 0) return 0;
    if (!front) { rib_clip(it, v, a, s, o); return 1; }
    o->n = 4;
    for (int k = 0; k < 4; k++)
        rib_corner_xy(s[k].sx, s[k].sy, a, (double)it->c[k].age, it->count, k, &o->x[k], &o->y[k]);
    tr_poly_attrs(&it->p, o);
    return 1;
}

/** @brief Builds an item's corners in float under a view at a sub-tick phase.
 *
 * Every corner is placed from its point as the present shows it; an item
 * reaching in front of the near floor is clipped against the near plane in
 * camera space (msh_build, rib_build), so no corner takes another build's
 * position.
 * @param it The item.
 * @param v The view.
 * @param alpha The present's position in the tick interval; 1 builds the tick.
 * @param o Receives the build.
 * @return 1 when built, 0 for an item with no source or a corner with no
 *         source point. */
static int tr_item_build(const tr_item* it, const ngte_view* v, double alpha, tr_poly* o) {
    if (it->src == TR_SRC_MESH) return msh_build(it, v, alpha, o);
    if (it->src == TR_SRC_RIBBON) return rib_build(it, v, alpha, o);
    return 0;
}

/* ---- the vertex probe (OPENPETE_TRACER_VTX) ------------------------------
 *
 * Printed at exit:
 *   * emu: each built corner of the routine's own primitive against an
 *     integer re-run of the routine's arithmetic on the same record, the
 *     calibration of the construction the float build follows.
 *   * cal: each corner of the routine's primitive minus the capture's float
 *     build, per axis; with each tick's mean and the count of ticks whose mean
 *     differs from the previous tick's.
 *   * place: per sub-tick present, the corner the feed placed before the float
 *     build (the routine's corner moved by its point's screen displacement)
 *     against the corner built at the state the present shows; on a canonical
 *     present the routine's corner against the capture's build.
 *   * jump: each corner at the alpha-0 present of tick N + 1 minus the same
 *     corner at the alpha-1 present of tick N, which show the same instant, for
 *     the fed and the pre-build forms; and ident, the alpha-1 present minus the
 *     canonical present of the same tick, fed form.
 *   * turning: sub-tick presents whose camera matrix differs from the tick's. */

typedef struct { unsigned long n; double sum, mn, mx; } tr_acc;

/** @brief Adds one sample to a running count, sum, minimum and maximum.
 * @param a The accumulator.
 * @param d The sample. */
static void tr_acc_add(tr_acc* a, double d) {
    if (a->n == 0 || d < a->mn) a->mn = d;
    if (a->n == 0 || d > a->mx) a->mx = d;
    a->sum += d;
    a->n++;
}

enum { TV_FED, TV_OLD, TV_N };

typedef struct {
    tr_acc        cal[2], tick[2], place[3], jump[TV_N], ident;
    tr_acc        mesh[2], centre[2], width;   /* cal split: mesh, ribbon point, ribbon half-width */
    unsigned long emu_n, emu_hit, ticks_cmp, ticks_chg, presents, turning, built, unbuilt;
    double        tsum[2];      /* the open tick's cal sums */
    unsigned long tn;
    double        last_mean[2];
    int           have_last;
    int           cam_M[3][3];  /* the tick's camera matrix */
} tr_vtx_t;
static tr_vtx_t s_vtx;

/* One corner's history, keyed by its point and side: the fed and pre-build
 * forms at the last canonical and alpha-1 presents, with their epochs. */
#define TR_VKEYS 8192
typedef struct {
    uint64_t key;
    unsigned canon_epoch, a1_epoch;
    float    canon[2], a1[TV_N][2];
} tr_vkey;
static tr_vkey s_vkey[TR_VKEYS];

/** @brief OPENPETE_TRACER_VTX, latched on first use: 0 off, 1 the exit summary.
 * @return The level. */
static int tr_vtx_level(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRACER_VTX"); v = (e && *e) ? atoi(e) : 0; }
    return v;
}

/** @brief Prints the mean, minimum and maximum of one accumulator on a stderr line.
 * @param name The line's label.
 * @param a The accumulator. */
static void tr_acc_print(const char* name, const tr_acc* a) {
    fprintf(stderr, "[tracer-vtx %s] n=%lu mean=%.6f min=%.6f max=%.6f\n", name, a->n,
            a->n ? a->sum / (double)a->n : 0.0, a->mn, a->mx);
}

/** @brief Prints the probe's summary to stderr; registered with atexit. */
static void tr_vtx_report(void) {
    OP_ROLE_ASSERT(sim);
    const tr_vtx_t* t = &s_vtx;
    fprintf(stderr, "[tracer-vtx emu] corners=%lu match=%lu built_items=%lu unbuilt_items=%lu\n",
            t->emu_n, t->emu_hit, t->built, t->unbuilt);
    tr_acc_print("cal x", &t->cal[0]);
    tr_acc_print("cal y", &t->cal[1]);
    tr_acc_print("tick_mean x", &t->tick[0]);
    tr_acc_print("tick_mean y", &t->tick[1]);
    fprintf(stderr, "[tracer-vtx tick change] compared=%lu changed=%lu\n", t->ticks_cmp, t->ticks_chg);
    tr_acc_print("cal mesh x", &t->mesh[0]);
    tr_acc_print("cal mesh y", &t->mesh[1]);
    tr_acc_print("cal ribbon point x", &t->centre[0]);
    tr_acc_print("cal ribbon point y", &t->centre[1]);
    tr_acc_print("cal ribbon half-width", &t->width);
    tr_acc_print("place canon", &t->place[0]);
    tr_acc_print("place inter", &t->place[1]);
    tr_acc_print("place inter turning", &t->place[2]);
    tr_acc_print("jump fed", &t->jump[TV_FED]);
    tr_acc_print("jump old", &t->jump[TV_OLD]);
    tr_acc_print("ident fed", &t->ident);
    fprintf(stderr, "[tracer-vtx turning] presents=%lu turning=%lu\n", t->presents, t->turning);
}

/** @brief Latches the probe and registers its summary on first use.
 * @return The level, 0 when the probe is off. */
static int tr_vtx_on(void) {
    static int armed = 0;
    const int lv = tr_vtx_level();
    if (lv > 0 && !armed) { armed = 1; atexit(tr_vtx_report); }
    return lv;
}

/** @brief Finds or claims the history slot of one corner key, overwriting a colliding key.
 * @param key The corner key.
 * @return The slot. */
static tr_vkey* tr_vkey_get(uint64_t key) {
    tr_vkey* e = &s_vkey[(key * 0x9E3779B97F4A7C15ull) >> 51];
    if (e->key != key) { memset(e, 0, sizeof *e); e->key = key; }
    return e;
}

/** @brief The history key of an item's corner: the point's record and age, or its vertex, and the corner's side.
 * @param it The item.
 * @param k The corner index.
 * @return The key. */
static uint64_t tr_corner_key(const tr_item* it, int k) {
    const tr_corner* c = &it->c[k];
    return ((uint64_t)c->key << 16) | ((uint64_t)it->src << 13) |
           ((uint64_t)(c->age & 0xFF) << 1) | (uint64_t)(k >= 2);
}

/** @brief Re-runs the ribbon routine's integer projection of one point.
 *
 * The halves, shifted left one, minus the camera >> 1 load as VZ0 and a packed
 * VXY0 whose low half borrows from the high (:2984-2999); RTPS under the
 * camera matrix with a zero translation; the packed SXY2 shifted left five and
 * back (:3019, :3113-3114).
 * @param v The tick's view.
 * @param w The point in full world units.
 * @param sz3 Receives SZ3.
 * @return The packed screen word the corners are offset from. */
static int32_t rib_emu_point(const ngte_view* v, const int32_t w[3], int32_t* sz3) {
    const int32_t t0 = w[0] / 2 - s_cam_half[0];
    const int32_t t1 = s_cam_half[1] - w[1] / 2;
    const int32_t t2 = s_cam_half[2] - w[2] / 2;
    const uint32_t xy = (uint32_t)t1 + ((uint32_t)t2 << 16);
    static const int k_tr0[3] = { 0, 0, 0 };
    ngte_rtps_out o;
    ngte_rtps_int(v->cam_M, k_tr0, (int16_t)(xy & 0xFFFFu), (int16_t)(xy >> 16), (int16_t)t0,
                  v->ofx, v->ofy, (int)v->H_canon, &o);
    *sz3 = o.sz3;
    const uint32_t sxy = ((uint32_t)(uint16_t)o.sx) | ((uint32_t)(uint16_t)o.sy << 16);
    return (int32_t)(sxy << 5) >> 5;
}

/** @brief Re-runs the ribbon routine's integer corners of one item.
 *
 * The head's half-width is the width byte << 11 over the head's SZ3 floored
 * at 0x80, a zero quotient made +1 or -1 (:3078-3092); a point's is that less
 * the product with its blend, age * (0x1000 / (count - 1)), shifted right 12,
 * placed in the packed word's high half while non-negative and sign-extended
 * into the low half otherwise (:3120-3136).
 * @param v The tick's view.
 * @param it The item.
 * @param ex Receives the corners' X.
 * @param ey Receives the corners' Y. */
static void rib_emu_corners(const ngte_view* v, const tr_item* it, int ex[4], int ey[4]) {
    int32_t sz;
    rib_emu_point(v, it->head.w, &sz);
    const int32_t num = (int32_t)((uint32_t)it->wb << 11);
    int32_t a2 = num / (sz < 0x80 ? 0x80 : sz);
    if (a2 == 0) a2 = (num >= 0) ? 1 : -1;
    const int32_t step = 0x1000 / (it->count - 1);
    for (int k = 0; k < 4; k++) {
        const int32_t xy = rib_emu_point(v, it->c[k].w, &sz);
        const uint32_t prod = (uint32_t)(a2 * it->c[k].age * step);
        int32_t at = (int32_t)(((uint32_t)a2 - (prod >> 12)) << 16);
        if (at < 0) at >>= 16;
        const uint32_t word = (k < 2) ? (uint32_t)xy - (uint32_t)at : (uint32_t)xy + (uint32_t)at;
        ex[k] = (int16_t)(word & 0xFFFFu);
        ey[k] = (int16_t)(word >> 16);
    }
}

/** @brief Re-runs the mesh routine's integer corners of one item.
 *
 * Each vertex's camera-relative offset is shifted right by the descriptor's
 * shift, packed masked into VXY0 with the X offset as VZ0, and RTPSed under
 * the camera matrix with a zero translation; the corner is SXY2
 * (:3227-3241).
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param it The item.
 * @param ex Receives the corners' X.
 * @param ey Receives the corners' Y. */
static void msh_emu_corners(CPUState* cpu, const ngte_view* v, const tr_item* it, int ex[4], int ey[4]) {
    int32_t cam[3];
    for (int k = 0; k < 3; k++) cam[k] = (int32_t)cpu->read_word(game_layout.cam.position + 4u * (uint32_t)k);
    const int sh = it->shift & 31;
    static const int k_tr0[3] = { 0, 0, 0 };
    for (int k = 0; k < 4; k++) {
        const int32_t* w = it->c[k].w;
        const int32_t a3 = (w[0] - cam[0]) >> sh;
        const int32_t t0 = (cam[1] - w[1]) >> sh;
        const int32_t t1 = (cam[2] - w[2]) >> sh;
        ngte_rtps_out o;
        ngte_rtps_int(v->cam_M, k_tr0, (int16_t)t0, (int16_t)t1, (int16_t)a3,
                      v->ofx, v->ofy, (int)v->H_canon, &o);
        ex[k] = o.sx;
        ey[k] = o.sy;
    }
}

/** @brief Closes the open tick of the probe's calibration: its mean per axis and whether it changed from the previous tick's. */
static void tr_vtx_tick_close(void) {
    tr_vtx_t* t = &s_vtx;
    if (t->tn == 0) return;
    const double m[2] = { t->tsum[0] / (double)t->tn, t->tsum[1] / (double)t->tn };
    for (int a = 0; a < 2; a++) tr_acc_add(&t->tick[a], m[a]);
    if (t->have_last) {
        t->ticks_cmp++;
        if (fabs(m[0] - t->last_mean[0]) > 1e-4 || fabs(m[1] - t->last_mean[1]) > 1e-4) t->ticks_chg++;
    }
    t->last_mean[0] = m[0]; t->last_mean[1] = m[1];
    t->have_last = 1;
    t->tsum[0] = t->tsum[1] = 0.0;
    t->tn = 0;
}

/** @brief Splits one built item's calibration error by its source: the mesh vertex, or the ribbon point and half-width.
 *
 * A ribbon segment's corners 0 and 2 straddle one point by its half-width
 * along one screen axis, so their mean is the point and half their
 * difference, summed over both axes, the half-width.
 * @param it The item. */
static void tr_vtx_split(const tr_item* it) {
    tr_vtx_t* t = &s_vtx;
    const float* px = it->p.x;
    const float* py = it->p.y;
    if (it->src == TR_SRC_MESH) {
        for (int k = 0; k < 4; k++) {
            tr_acc_add(&t->mesh[0], (double)px[k] - it->b.x[k]);
            tr_acc_add(&t->mesh[1], (double)py[k] - it->b.y[k]);
        }
        return;
    }
    for (int k = 0; k < 2; k++) {
        tr_acc_add(&t->centre[0], 0.5 * (((double)px[k] + px[k + 2]) - ((double)it->b.x[k] + it->b.x[k + 2])));
        tr_acc_add(&t->centre[1], 0.5 * (((double)py[k] + py[k + 2]) - ((double)it->b.y[k] + it->b.y[k + 2])));
        const double g = ((double)px[k + 2] - px[k]) + ((double)py[k + 2] - py[k]);
        const double b = ((double)it->b.x[k + 2] - it->b.x[k]) + ((double)it->b.y[k + 2] - it->b.y[k]);
        tr_acc_add(&t->width, 0.5 * (g - b));
    }
}

/** @brief Calibrates one call's items at capture and records their canonical corners.
 *
 * Every built corner adds its routine-minus-build error to cal and the open
 * tick, the integer re-run's agreement with the routine's corner to emu,
 * the routine-minus-build distance to the canonical place statistic, and the
 * build to the corner's canonical history.
 * @param cpu The guest CPU state.
 * @param v The tick's view.
 * @param set The call's items.
 * @param n The item count. */
static void tr_vtx_capture(CPUState* cpu, const ngte_view* v, const tr_item* set, int n) {
    if (!tr_vtx_on()) return;
    tr_vtx_t* t = &s_vtx;
    memcpy(t->cam_M, v->cam_M, sizeof t->cam_M);
    for (int i = 0; i < n; i++) {
        const tr_item* it = &set[i];
        if (!it->built || it->b.near) { t->unbuilt++; continue; }
        t->built++;
        if (it->p.nv == 4) tr_vtx_split(it);
        int ex[4], ey[4];
        if (it->src == TR_SRC_RIBBON) rib_emu_corners(v, it, ex, ey);
        else msh_emu_corners(cpu, v, it, ex, ey);
        for (int k = 0; k < it->p.nv && k < 4; k++) {
            t->emu_n++;
            if ((float)ex[k] == it->p.x[k] && (float)ey[k] == it->p.y[k]) t->emu_hit++;
            const double dx = (double)it->p.x[k] - it->b.x[k], dy = (double)it->p.y[k] - it->b.y[k];
            tr_acc_add(&t->cal[0], dx);
            tr_acc_add(&t->cal[1], dy);
            t->tsum[0] += dx; t->tsum[1] += dy; t->tn++;
            tr_acc_add(&t->place[0], hypot(dx, dy));
            tr_vkey* e = tr_vkey_get(tr_corner_key(it, k));
            e->canon_epoch = s_epoch;
            e->canon[0] = it->b.x[k]; e->canon[1] = it->b.y[k];
        }
    }
}

/** @brief Adds one corner of one sub-tick present to the place, jump and identity statistics.
 *
 * The pre-build form is the routine's corner moved by its point's screen
 * displacement from the tick's projection to the shown one (held where the
 * shown point has no screen position).
 * @param it The item.
 * @param k The corner index.
 * @param v The present's view.
 * @param alpha The present's position in the tick interval.
 * @param turning Nonzero when the present's camera matrix differs from the tick's. */
static void tr_vtx_corner(const tr_item* it, int k, const ngte_view* v, double alpha, int turning) {
    tr_vtx_t* t = &s_vtx;
    const float* fx = s_reb_active ? it->pp.x : it->b.x;
    const float* fy = s_reb_active ? it->pp.y : it->b.y;
    float f[TV_N][2] = { { fx[k], fy[k] }, { it->p.x[k], it->p.y[k] } };
    ngte_proj o;
    if (alpha < 1.0 && it->c[k].ok && tr_shown(it->src, &it->c[k], v, alpha, &o) &&
        o.m3 >= TR_NEAR_M3) {
        f[TV_OLD][0] += (float)(o.sx - it->c[k].sx);
        f[TV_OLD][1] += (float)(o.sy - it->c[k].sy);
    }
    const double d = hypot((double)f[TV_OLD][0] - f[TV_FED][0], (double)f[TV_OLD][1] - f[TV_FED][1]);
    tr_acc_add(&t->place[1], d);
    if (turning) tr_acc_add(&t->place[2], d);
    tr_vkey* e = tr_vkey_get(tr_corner_key(it, k));
    if (alpha <= 0.0 && e->a1_epoch != 0 && e->a1_epoch + 1u == s_epoch)
        for (int m = 0; m < TV_N; m++)
            tr_acc_add(&t->jump[m], hypot((double)f[m][0] - e->a1[m][0], (double)f[m][1] - e->a1[m][1]));
    if (alpha >= 1.0 && e->canon_epoch == s_epoch)
        tr_acc_add(&t->ident, hypot((double)f[TV_FED][0] - e->canon[0], (double)f[TV_FED][1] - e->canon[1]));
    if (alpha >= 1.0) { e->a1_epoch = s_epoch; memcpy(e->a1, f, sizeof e->a1); }
}

/** @brief Adds one sub-tick present's built corners to the probe (tr_vtx_corner) and counts a turning camera.
 * @param v The present's view.
 * @param alpha The present's position in the tick interval. */
static void tr_vtx_present(const ngte_view* v, double alpha) {
    if (!tr_vtx_on()) return;
    tr_vtx_t* t = &s_vtx;
    t->presents++;
    const int turning = memcmp(t->cam_M, v->cam_M, sizeof t->cam_M) != 0;
    t->turning += (unsigned long)turning;
    for (int i = 0; i < s_tick_n; i++) {
        const tr_item* it = &s_tick[i];
        if (!it->built || it->b.near || (s_reb_active && it->pp.near)) continue;
        for (int k = 0; k < it->p.nv && k < 4; k++) tr_vtx_corner(it, k, v, alpha, turning);
    }
}

/** @brief Builds the call's items in float under the tick's view and calibrates them.
 *
 * An item that does not build (no source, or a corner with no source point)
 * feeds the routine's corners.
 * @param cpu The guest CPU state.
 * @param v The tick's view. */
static void tr_build_call(CPUState* cpu, const ngte_view* v) {
    for (int i = 0; i < s_item_n; i++) {
        tr_item* it = &s_item[i];
        it->built = tr_item_build(it, v, 1.0, &it->b);
    }
    tr_vtx_capture(cpu, v, s_item, s_item_n);
}

/* ---- sub-tick rebuild ----------------------------------------------------- */

/** @brief Reports whether the sub-tick rebuild is enabled; OPENPETE_TRACERS_INTERP=0 disables it.
 *
 * Disabled, the replay stays at the tick's build, the engagement leg of the
 * sub-tick harness. Latched on the first call.
 * @return 0 when OPENPETE_TRACERS_INTERP starts with '0', else 1. */
static int tr_interp_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRACERS_INTERP"); v = !(e && *e && *e == '0'); }
    return v;
}

/** @brief Prints the rebuild's run totals to stderr, registered with atexit under OPENPETE_TRACER_REBASE_DBG.
 *
 * The line carries the lerped and held point counts per producer and the
 * largest world step paired. A run whose lerped count is zero rebuilt
 * nothing against a previous point. */
static void tr_rebase_report(void) {
    OP_ROLE_ASSERT(sim);
    fprintf(stderr, "[tracer-rebase] ribbon lerped=%lu held=%lu mesh lerped=%lu held=%lu "
            "worldmax=%.3f\n",
            s_reb_stat[TR_SRC_RIBBON][0], s_reb_stat[TR_SRC_RIBBON][1],
            s_reb_stat[TR_SRC_MESH][0], s_reb_stat[TR_SRC_MESH][1], s_reb_worldmax);
}

/** @brief Builds every held item's corners for the pose this present shows.
 *
 * The staged camera resolves through the sub-tick view, so the view built
 * here is the interpolated transform the rest of the pass draws under. At
 * alpha 1 or above, with an empty set or with the rebuild disabled, the
 * rebuild stays off and the replay emits the capture's build. The first call
 * past those conditions latches OPENPETE_TRACER_REBASE_DBG and registers
 * tr_rebase_report.
 * @param v The present's view.
 * @param alpha The present's position in the tick interval, 0..1. */
static void tr_rebase_all(const ngte_view* v, double alpha) {
    s_reb_active = 0;
    if (alpha >= 1.0 || s_tick_n <= 0 || !tr_interp_enabled()) return;
    static int dbg = -1;
    if (dbg < 0) {
        const char* e = getenv("OPENPETE_TRACER_REBASE_DBG");
        dbg = (e && *e && *e != '0');
        if (dbg) atexit(tr_rebase_report);
    }
    for (int i = 0; i < s_tick_n; i++) {
        tr_item* it = &s_tick[i];
        if (!it->built) continue;
        if (!tr_item_build(it, v, alpha, &it->pp)) it->pp.vz = it->b.vz;
        if (s_sac_live) tr_sac_depth(it, alpha);
    }
    s_reb_active = 1;
}

/* Called from each game's reextract.c on a sub-tick present. */
void tracers_reextract(CPUState* cpu, int bracketed, double alpha, int hold) OP_REQUIRES(sim) {
    const int pick = cut_held_sets_enter(&s_sets, hold);
    if (hook_reextract_ready_held(RP_TRACER, pick)) {
        /* On a held cut the staged camera is tick N-1's, the camera the shown
         * set was built under: alpha 1 emits the capture's build. */
        const double a = (bracketed && pick < 0) ? alpha : 1.0;
        ngte_view v;
        ngte_view_build(cpu, &v);
        s_sac_live = sac_level() > 0 && bracketed && pick < 0;
        if (s_sac_live) sac_present_begin(SAC_RIBBON, a);
        if (sac_level() > 0 && !bracketed && pick < 0 && s_tick_n > 0) sac_unbracketed_present(SAC_RIBBON);
        tr_rebase_all(&v, a);
        s_sac_live = 0;
        if (pick < 0) tr_vtx_present(&v, a);
        render_material_level_push(cpu);
        tr_feed_native(s_tick, s_tick_n);
        s_reb_active = 0;
    }
    cut_held_sets_leave(&s_sets, pick);
}

/* The hook addresses are the layout's two tracer draw sites, bound at boot: a
 * static initializer cannot read another TU's const. A game with no located
 * routine leaves either 0 and the registry dispatches nothing for it. */
static feature_dispatch_entry_t tracers_dispatch[] = {
    { 0u, on_ribbon, 0u },
    { 0u, on_mesh, 0u },
    { 0u, NULL, 0u },
};

/** @brief Binds the two dispatch rows to the layout's ribbon and mesh hook addresses.
 *
 * @param opts Unused. */
static void tracers_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    tracers_dispatch[0].addr = game_layout.tracers.ribbon_hook;
    tracers_dispatch[1].addr = game_layout.tracers.mesh_hook;
}

const feature_module_t tracers_module = {
    .name           = "tracers_native",
    .apply_options  = tracers_apply_options,
    .dispatch_table = tracers_dispatch,
};
