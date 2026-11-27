// games/common/renderer/player.c - the shared player-avatar feeder: the draw
// hook and its polybuf bracket, the per-call record store, the skeleton decode
// and projection, the face-stream walk, the scene feed and the sub-tick
// re-extract. The contract for the per-game half is renderer/player_game.h and
// every engine address comes from game_layout.player.
//
// The avatar is not a moby: a dedicated routine renders it from a dedicated
// state block, and its geometry is a skeleton of parts whose per-part
// transform records that routine builds in the tick's render scratch. Each
// part's vertices are delta-accumulated across two keyframe streams and blended
// by the part record's weight, and the parts stitch into one mesh whose faces
// index the whole projection.
//
// Determinism: the hook calls the game's own draw exactly once and only READS
// state afterwards; no CPU-path store to PSX RAM happens here. The one write
// through a host pointer is the re-extract's swap of a guest part block, made
// through openpete_subtick_host_span (the view's staged copy while a staged
// span covers the block) and restored before the feed. The record store copies
// guest bytes into host copies on the game fiber; the present side hands the
// copies to the re-extract, which stages them through the sub-tick view.

#include "feature_registry.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_psx.h"
#include "openpete_subtick_view.h"
#include "openpete_interp.h"
#include "openpete_fiber.h"          /* openpete_frame, openpete_speculative */
#include "openpete.h"                /* openpete_runahead_get */
#include "openpete_profile.h"
#include "game_layout.h"
#include "renderer/orchestrator.h"
#include "renderer/interp_regions_gen.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "renderer/moby.h"           /* the shared NCLIP, emit tree and tiledef decode */
#include "renderer/player.h"
#include "renderer/player_game.h"
#include "renderer/ui_game.h"         /* ui_sprite_player_link_note */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "renderer/env_knob.h"
#include "renderer/cut_guard.h"     /* cut_census_held_set */
#include "renderer/player_hide.h"
#include "renderer/water_plane.h"
#include "openpete_thread_roles.h"
#include "openpete_interp_objs.h"   /* g_openpete_tick_stamp */

extern unsigned openpete_frame;
extern op_time_t g_openpete_tick_period_ns;  /* one wall-time tick */

#define PL (game_layout.player)

/* ---- the part-transform block ------------------------------------------ */
uint32_t player_part_block_base(CPUState* cpu) {
    if (PL.part_base) return PL.part_base + PL.part_off;
    if (!game_layout.polybuf.end || !PL.part_base_polybuf_off) return 0;
    uint32_t end = cpu->read_word(game_layout.polybuf.end);
    if (end < 0x80000000u) return 0;
    return end - PL.part_base_polybuf_off + PL.part_off;
}

/* ---- the part records the walk reads -----------------------------------
 * The block the draw leaves in its render scratch is the default source. A
 * draw that consumes the block and then reuses those bytes as scratch inside
 * the same call answers `part_record` instead, and the walk composes the same
 * words from gamestate into this host copy: the block the hook could read back
 * would be whatever the later pass wrote there. The copy carries the game's
 * own block layout, so the record store, the prev/curr pair and the walk all
 * keep reading one shape. */
static uint8_t s_blk[PLAYER_REC_PART_MAX];
static int     s_blk_pinned;   /* the replay supplied the bytes; do not re-derive */

/** @brief Stores a 32-bit word little-endian into a byte buffer. */
static void blk_put(uint8_t* d, uint32_t v) {
    d[0]=(uint8_t)v; d[1]=(uint8_t)(v>>8); d[2]=(uint8_t)(v>>16); d[3]=(uint8_t)(v>>24);
}
/** @brief Loads a 32-bit little-endian word from a byte buffer. */
static uint32_t blk_get(const uint8_t* d) {
    return (uint32_t)d[0] | ((uint32_t)d[1]<<8) | ((uint32_t)d[2]<<16) | ((uint32_t)d[3]<<24);
}

static int player_parts(CPUState* cpu);

/** @brief Composes the whole part-transform block from gamestate into `dst`.
 *
 * The block is zeroed, then each part's flags word and A/B stream words are
 * written at the game's own offsets from the provider's part_record, and part
 * 0's seam words at part_seam_off when the layout declares it. A part the
 * provider refuses, or one past part_block_len, ends the block, which the zero
 * flags word already spells. A part_block_len over PLAYER_REC_PART_MAX
 * leaves the first PLAYER_REC_PART_MAX bytes zeroed, an empty block, and
 * writes nothing past them.
 * @param cpu Passed to the provider.
 * @param dst Receives part_block_len bytes, at most PLAYER_REC_PART_MAX. */
static void player_block_derive(CPUState* cpu, uint8_t* dst) {
    if (PL.part_block_len > PLAYER_REC_PART_MAX) {
        memset(dst, 0, PLAYER_REC_PART_MAX);
        return;
    }
    memset(dst, 0, PL.part_block_len);
    if (!player_game.part_record) return;
    const int n = player_parts(cpu);
    for (int p = 0; p < n; p++) {
        uint32_t rel = (uint32_t)p * PL.part_stride;
        if (rel + PL.part_stride > PL.part_block_len) break;
        player_record_t r; memset(&r, 0, sizeof r);
        if (!player_game.part_record(cpu, p, &r)) break;
        blk_put(dst + rel + PL.part_flags_off, r.flags);
        for (int k = 0; k < 3; k++) {
            blk_put(dst + rel + PL.part_stream_a_off + (uint32_t)k*4u, r.stream_a[k]);
            blk_put(dst + rel + PL.part_stream_b_off + (uint32_t)k*4u, r.stream_b[k]);
        }
        if (p == 0 && PL.part_seam_off && PL.part_seam_off + 8u <= PL.part_block_len) {
            blk_put(dst + PL.part_seam_off,      r.seam[0]);
            blk_put(dst + PL.part_seam_off + 4u, r.seam[1]);
        }
    }
}

/** @brief One word of a part's record, from the derived copy or from PSX RAM.
 *
 * Where the provider answers part_record the word comes from the derived host
 * copy s_blk and reads 0 past part_block_len; otherwise it is read from the
 * guest block.
 * @param cpu Reads the guest block.
 * @param block The guest block base; unused under part_record.
 * @param part The part index.
 * @param off The word's offset inside the part's record.
 * @return The word. */
static uint32_t player_rec_word(CPUState* cpu, uint32_t block, int part, uint32_t off) {
    uint32_t rel = (uint32_t)part * PL.part_stride + off;
    if (player_game.part_record)
        return (rel + 4u <= PL.part_block_len) ? blk_get(s_blk + rel) : 0u;
    return cpu->read_word(block + rel);
}

uint32_t player_rec_seam_word(int k) {
    if (!player_game.part_record || !PL.part_seam_off || k < 0 || k > 1) return 0u;
    uint32_t off = PL.part_seam_off + (uint32_t)k * 4u;
    return (off + 4u <= PL.part_block_len) ? blk_get(s_blk + off) : 0u;
}

/** @brief The part count the walk iterates.
 *
 * The provider's part_count when it has one, else the layout's part_count,
 * clamped to 0..PLAYER_PARTS_MAX.
 * @param cpu Passed to the provider.
 * @return The clamped count. */
static int player_parts(CPUState* cpu) {
    int n = player_game.part_count ? player_game.part_count(cpu) : PL.part_count;
    if (n > PLAYER_PARTS_MAX) n = PLAYER_PARTS_MAX;
    return n < 0 ? 0 : n;
}

/* ---- sub-tick keyframe-blend interpolation -----------------------------
 * A part's vertices blend keyframe A to B by the weight its record carries,
 * and that record is built at the canonical tick rate, so without a sub-tick
 * blend the animation steps at the tick rate while movement is smooth. The
 * orchestrator rolls prev/curr copies of the whole part block at the canonical
 * hook and hands the pair here; the re-extract decodes the two REAL rendered
 * poses and lerps the projected vertices. Decoding one mixed block instead
 * would interpolate at blends the game never rendered, which whips
 * seam-relative vertices off-model whenever a part advances a whole keyframe
 * in a tick, and the cumulative vertex counts of a chained block would desync
 * besides. */
static const uint8_t* s_seg_prev = NULL;
static const uint8_t* s_seg_curr = NULL;
static int     s_seg_valid = 0;       /* 0 none, 1 only curr, 2 prev+curr */
static double  s_anim_alpha = 0.0;
static int     s_anim_on    = 0;
static int     s_player_slot = 0;     /* record index k: the PLAYER tag slot */

void player_native_set_anim_interp(double alpha, int on) { s_anim_alpha = alpha; s_anim_on = on ? 1 : 0; }
void player_native_set_anim_pair(const uint8_t* prev_seg, const uint8_t* curr_seg) {
    s_seg_prev = prev_seg; s_seg_curr = curr_seg;
    s_seg_valid = curr_seg ? (prev_seg ? 2 : 1) : 0;
}
void player_native_set_player_slot(int k) { s_player_slot = k < 0 ? 0 : k; }

void player_view_mac(CPUState* cpu, int32_t d[3], int32_t mac[3]) {
    const uint32_t rec = PL.base, cam = game_layout.cam.base;
    d[0] = (int32_t)(cpu->read_word(rec)       - cpu->read_word(cam + 0x28u));
    d[1] = (int32_t)(cpu->read_word(cam + 0x2Cu) - cpu->read_word(rec + 4u));
    d[2] = (int32_t)(cpu->read_word(cam + 0x30u) - cpu->read_word(rec + 8u));
    const int64_t ir[3] = { (int16_t)d[1], (int16_t)d[2], (int16_t)d[0] };
    int16_t r[9];
    for (int k = 0; k < 9; k++)
        r[k] = (int16_t)cpu->read_half(cam + 2u * (uint32_t)k);
    for (int row = 0; row < 3; row++)
        mac[row] = (int32_t)((r[row*3] * ir[0] + r[row*3 + 1] * ir[1] + r[row*3 + 2] * ir[2]) >> 12);
}

/* The front-layer flatten (the sort-depth sentinel) pulls the model onto the
 * near plane, capped so the nearest vertex lands ON the near floor. With N
 * avatars per tick a per-CALL cap flattens each to the floor independently and
 * the z-test interleaves their bodies. The cap is therefore shared across the
 * calls of one tick: the running minimum over the tick's calls so far, and -
 * for the first calls, before the nearest avatar has drawn - the previous
 * tick's all-calls minimum when that tick had more than one call. One call per
 * tick is the call's own minimum. */
static struct { uint32_t key; int calls; double min; } s_flat_cur = { 0, 0, 1e30 }, s_flat_prev = { 0, 0, 1e30 };
void player_native_set_flat_key(uint32_t key) {
    if (s_flat_cur.key == key) return;
    s_flat_prev = s_flat_cur;
    s_flat_cur.key = key; s_flat_cur.calls = 0; s_flat_cur.min = 1e30;
}
/** @brief The near-plane cap the front-layer flatten shares across a tick's calls.
 *
 * Counts the call into the current group and folds `own_min` into the group's
 * running minimum. The result is that minimum, lowered to the previous group's
 * minimum when the previous group held more than one call, so the first calls
 * of a tick, drawn before the nearest avatar, cap at the value the whole
 * previous group settled on.
 * @param own_min This call's smallest positive view-Z, or 1e30 when none.
 * @return The cap. */
static double player_flat_min(double own_min) {
    s_flat_cur.calls++;
    if (own_min < s_flat_cur.min) s_flat_cur.min = own_min;
    double m = s_flat_cur.min;
    if (s_flat_prev.calls > 1 && s_flat_prev.min < m) m = s_flat_prev.min;
    return m;
}

/* Sub-tick ROTATION interpolation: per-part float byte-angles (axis k = 0 Z /
 * 1 X / 2 Y) the orchestrator interpolated from the prev/curr euler snapshots,
 * so the composer renders rotation below the 8-bit euler grid (256/circle)
 * instead of stepping on it. s_rot_active[p] gates each part. */
static int    s_rot_active[PLAYER_PARTS_MAX] = {0,0,0,0};
static double s_rot_ang[PLAYER_PARTS_MAX][3];
void player_native_set_rot_interp(const int* active, const double* ang) {
    if (!active) { for (int p = 0; p < PLAYER_PARTS_MAX; p++) s_rot_active[p] = 0; return; }
    for (int p = 0; p < PLAYER_PARTS_MAX; p++) {
        s_rot_active[p] = active[p];
        for (int k = 0; k < 3; k++) s_rot_ang[p][k] = ang[p*3 + k];
    }
}
static int s_mirror_pass;
void player_native_set_mirror(int on) { s_mirror_pass = on ? 1 : 0; }
int  player_mirror_pass(void) { return s_mirror_pass; }

int player_rot_interp(int part, double out[3]) {
    if (part < 0 || part >= PLAYER_PARTS_MAX || !s_rot_active[part]) return 0;
    for (int k = 0; k < 3; k++) out[k] = s_rot_ang[part][k];
    return 1;
}

/* ---- the water-plane split ---------------------------------------------
 * The draw rewrites every projected vertex's staged word with a signed
 * distance to a world plane and classifies each face by the signs of its own
 * vertices' distances (the OR admits a face wholly on the positive side, the
 * AND separates a wholly-negative face from a straddling one). The negative
 * half is dropped, or depth-cued toward the environment's far colour by the
 * plane's own IR0; a straddling face is cut at the plane and the two halves
 * take the two policies. The provider answers the plane; `active` 0 leaves
 * every number below untouched. */
static player_plane_t s_plane;

/** @brief The signed distance from a decoded model vertex to the water plane.
 *
 * Evaluates plane - floor((row . V) / 4096 + tr[part]) from s_plane, the
 * floored form the draw's own MVMVA yields.
 * @param part The part the vertex belongs to.
 * @param VX The decoded model-space X.
 * @param VY The decoded model-space Y.
 * @param VZ The decoded model-space Z.
 * @return The distance; 0 when `part` is outside 0..PLAYER_PARTS_MAX - 1. */
static double plane_dist(int part, double VX, double VY, double VZ) {
    if (part < 0 || part >= PLAYER_PARTS_MAX) return 0.0;
    return s_plane.plane
         - floor((s_plane.row[0]*VX + s_plane.row[1]*VY + s_plane.row[2]*VZ) / 4096.0
                 + s_plane.tr[part]);
}

/** @brief The water plane's negative-half colour for one packed 0x00BBGGRR vertex colour.
 *
 * The shared DPCS chain (wplane_shade) at the plane's far colour and IR0.
 * @param rgb The vertex colour.
 * @return The cued colour. */
static uint32_t plane_shade(uint32_t rgb) {
    return wplane_shade(s_plane.fc, s_plane.ir0, rgb);
}

/** @brief Cues one packed 0x00BBGGRR colour toward a tint's far colour, the draw's DPCS pass over the model colour table.
 *
 * The draw loads the far colour's channels shifted left by 4 into RFC/GFC/BFC
 * and the weight byte shifted left by 4 into IR0, then runs one sf = 1 DPCS per
 * table entry. Both of that chain's shifts are arithmetic and its divisors
 * powers of two, so the two floors compose into one:
 *   out = floor(c + (f - c) * w / 256)
 * with c the channel, f the far channel and w the weight byte, clamped to a
 * byte. The float form is exact: (f - c) * w is at most 65025 in magnitude and
 * the divide by 256 only moves the exponent.
 * @param rgb The model colour.
 * @param tint The tint word; byte 3 the weight, bytes 0-2 the far colour.
 * @return The cued colour. */
static uint32_t player_tint_cue(uint32_t rgb, uint32_t tint) {
    const float w = (float)(tint >> 24);
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        const float ch = (float)((rgb  >> (8*i)) & 0xFFu);
        const float fc = (float)((tint >> (8*i)) & 0xFFu);
        float v = floorf(ch + (fc - ch) * w / 256.0f);
        if (v < 0.0f) v = 0.0f; else if (v > 255.0f) v = 255.0f;
        out |= (uint32_t)v << (8*i);
    }
    return out;
}

/** @brief Whether OPENPETE_PLAYER_VERIFY_NOTINT, the tint cue's negative control, is set.
 *
 * Latched on the first call. When set, player_face_colour returns every
 * colour block entry uncued, so on a tick whose tint weight is non-zero the
 * verify probe's colour leg collapses.
 * @return 1 when set, else 0. */
static int player_tint_disabled(void) {
    static int v = -1;
    ENV_FLAG_LATCH(v, "OPENPETE_PLAYER_VERIFY_NOTINT");
    return v;
}

/** @brief One face vertex's colour: the colour block's entry at a byte offset, cued by the block's tint.
 *
 * A tint whose weight byte is zero, or OPENPETE_PLAYER_VERIFY_NOTINT, leaves
 * the entry as read.
 * @param cpu Reads the colour block.
 * @param F The part's streams and tint.
 * @param off The entry's byte offset in the block.
 * @return The packed 0x00BBGGRR colour. */
static uint32_t player_face_colour(CPUState* cpu, const player_faces_t* F, uint32_t off) {
    uint32_t c = cpu->read_word(F->colours + off) & 0xFFFFFFu;
    return ((F->tint >> 24) && !player_tint_disabled()) ? player_tint_cue(c, F->tint) : c;
}

/** @brief Whether OPENPETE_PLAYER_VERIFY_NOPLANE, the water-plane split's negative control, is set.
 *
 * Latched on the first call. When set, player_project_all never asks the
 * provider for the plane, so every face takes the unsplit path; on a pose the
 * draw splits, the verify probe's plane and colour legs collapse.
 * @return 1 when set, else 0. */
static int player_plane_disabled(void) {
    static int v = -1;
    ENV_FLAG_LATCH(v, "OPENPETE_PLAYER_VERIFY_NOPLANE");
    return v;
}

/* ---- projection -------------------------------------------------------- */
/** @brief Projects one model-space vertex through a part transform with the GTE's clamps in float.
 *
 * Runs ngte_project_gte with the SXY bound on. For a vertex close to the
 * camera the hardware caps the perspective divide, which pins a near vertex
 * near the screen centre; the true, larger reciprocal would throw such a
 * vertex off the screen edge. Mirroring the cap and the IR16 and SXY clamps
 * leaves every in-range vertex equal to the hardware's, where the quotient is
 * exact. The UNR reciprocal's sub-pixel error is not reproduced.
 * @param M The part rotation at unit scale.
 * @param TR The part translation.
 * @param vx The vertex's X.
 * @param vy The vertex's Y.
 * @param vz The vertex's Z.
 * @param ofx_px Screen X offset in pixels.
 * @param ofy_px Screen Y offset in pixels.
 * @param v The view.
 * @param out_sx Receives the screen X.
 * @param out_sy Receives the screen Y.
 * @param out_sat Receives the saturation flag (divide capped or SZ3 not positive); may be NULL.
 * @return MAC3, the vertex's view-Z. */
static double project(const float M[3][3], const double TR[3], double vx, double vy, double vz,
                      double ofx_px, double ofy_px, const ngte_view* v,
                      double* out_sx, double* out_sy, int* out_sat) {
    ngte_proj o;
    ngte_project_gte_f(M, TR, vx, vy, vz, ofx_px, ofy_px, v, 1, &o);
    *out_sx = o.sx;
    *out_sy = o.sy;
    if (out_sat) *out_sat = o.sat;
    return o.m3;
}

/* OPENPETE_PLAYER_VERIFY_BITSHIFT: the negative control of the probe below.
 * Reading the model-space X one bit over is what a mis-placed field boundary
 * in the vertex stream does, and it must collapse the probe's precision; a
 * probe that still passes with it armed is measuring nothing. */
static int s_probe_bitshift;

typedef struct { float sx, sy, vz; int sat, seg; double pd; } spxy;
#define MAX_SPROJ 2048
static spxy s_sproj[MAX_SPROJ]; static int s_sproj_n;
static spxy s_sproj_prev[MAX_SPROJ];   /* prev-pose projection for the two-pass shape lerp */

void player_proj_vert(int i, float* sx, float* sy, float* vz, int* sat, int* part) {
    if (i < 0 || i >= s_sproj_n) return;
    if (sx) *sx = s_sproj[i].sx;
    if (sy) *sy = s_sproj[i].sy;
    if (vz) *vz = s_sproj[i].vz;
    if (sat) *sat = s_sproj[i].sat;
    if (part) *part = s_sproj[i].seg;
}

/** @brief Decodes one part's vertices from its two keyframe streams, projects each and appends it to s_sproj.
 *
 * The stream cursors and each stream's opening form come from the part record
 * at the layout's offsets; the per-vertex step is the provider's
 * vstream_next. When the flags word's blend bits are nonzero both streams
 * advance and the vertex is A + blend/4096 * (B - A); otherwise stream A
 * alone. Each entry records its part and, when the water plane is active, its
 * signed plane distance. Stops at MAX_SPROJ entries.
 * @param cpu Reads the streams.
 * @param block The part-transform block's guest base.
 * @param M The part rotation at unit scale.
 * @param TR The part translation.
 * @param ofx Screen X offset in pixels.
 * @param ofy Screen Y offset in pixels.
 * @param v The view.
 * @param part The part index.
 * @param count The part's vertex count.
 * @return The vertices appended. */
static int player_project_part(CPUState* cpu, uint32_t block, const float M[3][3], const double TR[3],
                               double ofx, double ofy, const ngte_view* v, int part, int count) {
    uint32_t fd = player_rec_word(cpu, block, part, PL.part_flags_off);
    uint32_t blend_raw = fd & PL.blend_mask;
    int dual = (blend_raw != 0);
    double blend = (double)blend_raw / 4096.0;

    moby_vstream_t A, B;
    memset(&A, 0, sizeof A); memset(&B, 0, sizeof B);
    A.scratch_u[0] = player_rec_word(cpu, block, part, PL.part_stream_a_off);
    A.litp         = player_rec_word(cpu, block, part, PL.part_stream_a_off + 4u);
    A.shortp       = player_rec_word(cpu, block, part, PL.part_stream_a_off + 8u);
    A.flag         = (int)((fd >> PL.part_stream_flag_bit[0]) & 1u);
    B.scratch_u[0] = player_rec_word(cpu, block, part, PL.part_stream_b_off);
    B.litp         = player_rec_word(cpu, block, part, PL.part_stream_b_off + 4u);
    B.shortp       = player_rec_word(cpu, block, part, PL.part_stream_b_off + 8u);
    B.flag         = (int)((fd >> PL.part_stream_flag_bit[1]) & 1u);

    int emitted = 0;
    for (int i = 0; i < count && s_sproj_n < MAX_SPROJ; i++) {
        player_game.vstream_next(cpu, &A, i == 0);
        double VX = A.VX, VY = A.VY, VZ = A.VZ;
        if (dual) {
            player_game.vstream_next(cpu, &B, i == 0);
            VX = (double)A.VX + blend * (B.VX - A.VX);
            VY = (double)A.VY + blend * (B.VY - A.VY);
            VZ = (double)A.VZ + blend * (B.VZ - A.VZ);
        }
        if (s_probe_bitshift) VX *= 2.0;   /* the negative control below */
        double sx, sy; int sat;
        double vz = project(M, TR, VX, VY, VZ, ofx, ofy, v, &sx, &sy, &sat);
        s_sproj[s_sproj_n].sx = (float)sx; s_sproj[s_sproj_n].sy = (float)sy;
        s_sproj[s_sproj_n].vz = (float)vz; s_sproj[s_sproj_n].sat = sat;
        s_sproj[s_sproj_n].seg = part;
        s_sproj[s_sproj_n].pd  = s_plane.active ? plane_dist(part, VX, VY, VZ) : 0.0;
        s_sproj_n++; emitted++;
    }
    return emitted;
}

/* ---- the skeleton's transforms ----------------------------------------
 * Derived from gamestate by the provider (the camera's projection matrix
 * against the avatar's pose, plus the seam joint that hangs each child part
 * off its parent), never read back from the record the draw built: the record
 * is the canonical tick's, and a sub-tick present needs the transform under
 * the interpolated camera. */
static player_xform_t s_xf[PLAYER_PARTS_MAX];

const player_xform_t* player_part_xform(int p) {
    return (p >= 0 && p < PLAYER_PARTS_MAX && s_xf[p].present) ? &s_xf[p] : NULL;
}

/** @brief Fills s_xf with every part's transform, root first, then seam-joined children.
 *
 * Clears s_xf, asks the provider for part 0 with no parent, and returns with
 * every entry absent when it has no part_xform, the part count is 0, or it
 * refuses the root. Each child is derived against the root; a child the
 * provider refuses stays absent, and an accepted one has its translation
 * replaced by the provider's seam_joint when it has one.
 * @param cpu Passed to the provider.
 * @param v The view the transforms are built under.
 * @param fd0 The root part's flags word; its blend bits give the keyframe
 *        weight the seam blends on, zero meaning a single keyframe. */
static void player_derive_xforms(CPUState* cpu, const ngte_view* v, uint32_t fd0) {
    memset(s_xf, 0, sizeof s_xf);
    const int n = player_parts(cpu);
    if (n <= 0 || !player_game.part_xform) return;
    if (!player_game.part_xform(cpu, v, 0, NULL, &s_xf[0])) return;
    /* The seam blends on the ROOT part's keyframe weight, not on a per-part
     * progress byte: the sub-tick re-extract writes a fine interpolated weight
     * into the root record, and a coarse per-part byte would step the seam
     * while the vertices around it stay smooth. */
    int ir0 = (int)(fd0 & PL.blend_mask);
    int dual = (ir0 != 0);
    for (int p = 1; p < n; p++) {
        if (!player_game.part_xform(cpu, v, p, &s_xf[0], &s_xf[p])) continue;
        if (player_game.seam_joint)
            player_game.seam_joint(cpu, &s_xf[0], p, ir0, dual, s_xf[p].TR);
    }
}

/* Walk the part records, decode and project every vertex. */
/* Per-tick engagement counts: a pass that decoded nothing proves nothing, so
 * the probe prints what this walk actually read. */
static long s_eng_parts, s_eng_verts, s_eng_faces, s_eng_tris;
/* Faces this walk classified, by plane class: above, below-shaded,
 * below-dropped, straddling. A split that engaged nothing is not a pass. */
static long s_plane_faces[4];

int player_project_all(CPUState* cpu) {
    s_sproj_n = 0;
    s_eng_parts = 0; s_eng_verts = 0;
    memset(&s_plane, 0, sizeof s_plane);
    if (!player_game.vstream_next) return 0;
    if (player_game.plane_begin && !player_plane_disabled())
        player_game.plane_begin(cpu, &s_plane);
    uint32_t block = player_part_block_base(cpu);
    if (!block && !player_game.part_record) return 0;
    if (player_game.part_record && !s_blk_pinned) player_block_derive(cpu, s_blk);
    ngte_view view; ngte_view_build(cpu, &view);
    const ngte_view* v = &view;
    player_derive_xforms(cpu, v, player_rec_word(cpu, block, 0, PL.part_flags_off));
    const int nparts = player_parts(cpu);
    int cursor = 0;                 /* the running value a cumulative count advances */
    int nseg = 0;
    for (int p = 0; p < nparts; p++) {
        uint32_t fd = player_rec_word(cpu, block, p, PL.part_flags_off);
        if (fd == 0) break;                          /* the block's terminator */
        int count = player_game.part_vcount ? player_game.part_vcount(fd, &cursor) : 0;
        if (count <= 0) continue;
        if (!s_xf[p].present) continue;
        s_eng_verts += player_project_part(cpu, block, (const float (*)[3])s_xf[p].Mf, s_xf[p].TR,
                                          v->ofx, v->ofy, v, p, count);
        s_eng_parts++;
        nseg++;
    }
    return nseg ? s_sproj_n : 0;
}

/* ---- the face-stream walk ---------------------------------------------
 * Topology, per-vertex colour and UVs come from the model's own face and
 * colour streams, decoded by the provider into the shared face record, and the
 * emit runs the dual-NCLIP tree the moby walk already owns. Nothing here reads
 * the primitives the game's draw emitted. */
typedef struct { float sx,sy,vz; int u,v; uint32_t rgb; int idx; } swv;
typedef struct {
    swv v[3];
    int textured, tpage_x, tpage_y, clut_x, clut_y, bpp, blend;
    int submerged;   /* the plane split's negative half */
} swtri;
#define MAX_SWTRI (MAX_SPROJ * 4)
#define MAX_SWFACE 8192
static swtri s_swtri[MAX_SWTRI]; static int s_swtri_n;
static moby_wface_t s_face[MAX_SWFACE];

void player_walk_tri(int k, int idx[3], uint32_t rgb[3], int u[3], int v[3]) {
    if (k < 0 || k >= s_swtri_n) return;
    for (int j = 0; j < 3; j++) {
        if (idx) idx[j] = s_swtri[k].v[j].idx;
        if (rgb) rgb[j] = s_swtri[k].v[j].rgb;
        if (u)   u[j]   = s_swtri[k].v[j].u;
        if (v)   v[j]   = s_swtri[k].v[j].v;
    }
}

/** @brief Signed 2*area of three projected stream vertices on the native float screen coordinates.
 *
 * The GTE's int16 grid flips a near-zero-area face's sign at grazing angles
 * and culls front-facing thin geometry (a tail tip, a wing edge), so the float
 * form is used. The Z-buffer sorts the survivors.
 * @param i0 First vertex index.
 * @param i1 Second vertex index.
 * @param i2 Third vertex index.
 * @return The area term, or -1 when an index is past the projected count. */
static double player_nclip(int i0, int i1, int i2) {
    if (i0 >= s_sproj_n || i1 >= s_sproj_n || i2 >= s_sproj_n) return -1.0;
    return moby_tri_nclip(s_sproj[i0].sx, s_sproj[i0].sy,
                          s_sproj[i1].sx, s_sproj[i1].sy,
                          s_sproj[i2].sx, s_sproj[i2].sy);
}

/** @brief The depth key the draw sums before its per-face admission.
 *
 * The sum is over the four vertices of a quad when the NCLIP tree emitted two
 * triangles, or, where it left one triangle, that triangle's three with the
 * middle vertex counted twice, so both shapes share one scale. Each term is
 * SZ3: the projected view-Z saturated to 0..0xFFFF, which is what the draw
 * reads back per vertex; an index past the projected count reads 0.
 * @param idx The face's four stream vertex indices.
 * @param order The emit tree's triangle orders; order[0] names the single triangle.
 * @param nemit Triangles the emit tree produced, at least 1.
 * @return The summed key. */
static double player_depth_sum(const uint16_t idx[4], const int order[2][3], int nemit) {
    double sz[4];
    int n = (nemit >= 2) ? 4 : 3;
    const int pos[4] = { nemit >= 2 ? 0 : order[0][0], nemit >= 2 ? 1 : order[0][1],
                         nemit >= 2 ? 2 : order[0][2], 3 };
    for (int k = 0; k < n; k++) {
        int i = idx[pos[k]];
        double z = (i < s_sproj_n) ? (double)s_sproj[i].vz : 0.0;
        sz[k] = z < 0.0 ? 0.0 : (z > 65535.0 ? 65535.0 : z);
    }
    return (nemit >= 2) ? (sz[0] + sz[1] + sz[2] + sz[3])
                        : (sz[0] + sz[1] + sz[1] + sz[2]);
}

/* The material one triangle carries, so the plane split can pass it along
 * without restating seven arguments per emit. */
typedef struct { int tex, tpx, tpy, clx, cly, bpp, blend; } swmat;

/* One vertex on the way to the scene: a projected stream vertex, or a point
 * the plane cut produced, which has no stream index of its own. The shared
 * cut's attribute slots carry view depth and the texture coordinates, and the
 * zero perspective divisor keeps the guest draw's screen-linear cut. */
typedef wplane_vert_t pclv;
enum { PCLV_VZ, PCLV_U, PCLV_V, PCLV_NAT };

/** @brief Appends one triangle to the walk's triangle store.
 *
 * Screen position and view-Z narrow to float and the texture coordinates
 * truncate to int. The triangle is dropped when the store holds MAX_SWTRI.
 * @param v The three vertices.
 * @param m The triangle's material.
 * @param submerged Nonzero for a triangle of the plane split's negative half. */
static void emit_raw(const pclv v[3], const swmat* m, int submerged) {
    if (s_swtri_n >= MAX_SWTRI) return;
    swtri* t = &s_swtri[s_swtri_n++];
    for (int k = 0; k < 3; k++)
        t->v[k] = (swv){ (float)v[k].sx, (float)v[k].sy, (float)v[k].at[PCLV_VZ],
                         (int)v[k].at[PCLV_U], (int)v[k].at[PCLV_V], v[k].rgb, v[k].idx };
    t->textured=m->tex; t->tpage_x=m->tpx; t->tpage_y=m->tpy;
    t->clut_x=m->clx; t->clut_y=m->cly; t->bpp=m->bpp; t->blend=m->blend;
    t->submerged = submerged;
}

/** @brief Cuts one triangle at the water plane and emits each half under its own policy.
 *
 * The positive half (plane distance >= 0) emits stock; the negative half is
 * shaded toward the far colour by plane_shade, or dropped under drop_below.
 * Each half is emitted as a fan. With the split off, or with every vertex on
 * the positive side, this is one emit of the triangle it was handed; with
 * every vertex on the negative side it is one emit of the shaded triangle, or
 * none under drop_below. The cut
 * interpolates screen position, UV and colour (rounded per channel), which is
 * what the draw's three INTPL passes interpolate, and view depth besides,
 * which the draw does not carry per vertex; a cut point takes the stream index
 * of the edge's first vertex.
 * @param in The triangle's three vertices with their plane distances.
 * @param m The triangle's material. */
static void plane_emit(const pclv in[3], const swmat* m) {
    if (!s_plane.active) { emit_raw(in, m, 0); return; }
    pclv pos[4], neg[4]; int np = 0, nn = 0;
    wplane_cut(in, PCLV_NAT, pos, &np, neg, &nn);
    for (int k = 2; k < np; k++) {
        pclv f[3] = { pos[0], pos[k-1], pos[k] };
        emit_raw(f, m, 0);
    }
    if (s_plane.drop_below) return;
    for (int k = 0; k < nn; k++) neg[k].rgb = plane_shade(neg[k].rgb);
    for (int k = 2; k < nn; k++) {
        pclv f[3] = { neg[0], neg[k-1], neg[k] };
        emit_raw(f, m, 1);
    }
}

/** @brief Emits one triangle over projected stream vertices through the plane split.
 *
 * Takes screen position, view-Z and plane distance from s_sproj and the rest
 * from the arguments. Nothing is emitted when the store is full or an index is
 * not below the projected count.
 * @param i0 First vertex's s_sproj index.
 * @param i1 Second vertex's s_sproj index.
 * @param i2 Third vertex's s_sproj index.
 * @param c0 First vertex colour, packed 0x00BBGGRR.
 * @param c1 Second vertex colour.
 * @param c2 Third vertex colour.
 * @param u0 First vertex texture U.
 * @param v0 First vertex texture V.
 * @param u1 Second vertex texture U.
 * @param v1 Second vertex texture V.
 * @param u2 Third vertex texture U.
 * @param v2 Third vertex texture V.
 * @param tex Nonzero for a textured triangle.
 * @param tpx Texture page X.
 * @param tpy Texture page Y.
 * @param clx CLUT X.
 * @param cly CLUT Y.
 * @param bpp Texture depth in bits per pixel.
 * @param blend The semitransparency mode from moby_abr_blend, 0 for opaque. */
static void emit_swtri(int i0,int i1,int i2, uint32_t c0,uint32_t c1,uint32_t c2,
                       int u0,int v0,int u1,int v1,int u2,int v2,
                       int tex,int tpx,int tpy,int clx,int cly,int bpp,int blend) {
    if (s_swtri_n >= MAX_SWTRI) return;
    if (i0 >= s_sproj_n || i1 >= s_sproj_n || i2 >= s_sproj_n) return;
    const int ii[3] = { i0, i1, i2 };
    const uint32_t cc[3] = { c0, c1, c2 };
    const int uu[3] = { u0, u1, u2 }, vv[3] = { v0, v1, v2 };
    pclv t[3];
    for (int k = 0; k < 3; k++) {
        int i = ii[k];
        t[k].sx = s_sproj[i].sx; t[k].sy = s_sproj[i].sy; t[k].w = 0.0;
        t[k].at[PCLV_VZ] = s_sproj[i].vz; t[k].at[PCLV_U] = uu[k]; t[k].at[PCLV_V] = vv[k];
        t[k].rgb = cc[k];
        t[k].pd = s_sproj[i].pd; t[k].idx = i;
    }
    const swmat m = { tex, tpx, tpy, clx, cly, bpp, blend };
    plane_emit(t, &m);
}

int player_build_walk(CPUState* cpu) {
    s_swtri_n = 0;
    s_eng_faces = 0;
    memset(s_plane_faces, 0, sizeof s_plane_faces);
    if (s_sproj_n <= 0 || !player_game.part_faces || !player_game.decode_faces) return 0;
    const int nparts = player_parts(cpu);
    /* A game whose whole skeleton shares one stitched mesh answers the same
     * streams for every part; decode each distinct pair once. */
    uint32_t seen_faces[PLAYER_PARTS_MAX]; int nseen = 0;
    for (int p = 0; p < nparts; p++) {
        player_faces_t F = {0};
        if (!player_game.part_faces(cpu, p, &F)) continue;
        if (F.faces < 0x80000000u || F.colours < 0x80000000u) continue;
        int dup = 0;
        for (int q = 0; q < nseen; q++) if (seen_faces[q] == F.faces) { dup = 1; break; }
        if (dup) continue;
        seen_faces[nseen++] = F.faces;

        uint32_t len = cpu->read_word(F.faces);
        if (len == 0 || len > 0x20000u) continue;
        moby_face_stats_t st;
        int nf = player_game.decode_faces(cpu, F.faces, len, s_face, MAX_SWFACE, &st);
        s_eng_faces += nf;
        for (int fi = 0; fi < nf; fi++) {
            const moby_wface_t* f = &s_face[fi];
            uint32_t c0 = player_face_colour(cpu, &F, f->c0);
            uint32_t c1 = player_face_colour(cpu, &F, f->c1);
            uint32_t c2 = player_face_colour(cpu, &F, f->c2);
            uint32_t c3 = player_face_colour(cpu, &F, f->c3);
            int blend = f->blendbit ? moby_abr_blend(f->abr) : 0;
            double n1 = player_nclip(f->i0, f->i1, f->i2);
            double n2 = (f->i3 < s_sproj_n) ? player_nclip(f->i3, f->i1, f->i2) : -1.0;
            /* The mirrored copy reflects the skeleton matrix, which reverses
             * every face's screen winding; the draw negates both NCLIP
             * results so the same faces survive the back-face test. */
            if (s_mirror_pass) { n1 = -n1; if (f->i3 < s_sproj_n) n2 = -n2; }
            int order[2][3];
            /* A game whose draw admits a flagged face untested takes the emit
             * tree's unconditional arm, which keeps the face's whole topology
             * without reading either NCLIP. */
            int uncond = player_game.face_flag_skips_nclip && f->untested;
            int fbit0 = uncond ? 1 : (player_game.face_winding_flag ? f->bit0 : 0);
            int nemit = moby_face_emit(f->fresh, fbit0,
                                       1 /* the flag exempts, it never re-tests */,
                                       n1, n2, f->i3 < s_sproj_n, order);
            const uint16_t idx[4] = { f->i0, f->i1, f->i2, f->i3 };
            const uint32_t col[4] = { c0, c1, c2, c3 };
            /* The plane classification runs BEFORE the depth admission, as the
             * draw's does: a face wholly on the negative side is dropped
             * outright where the policy drops, and otherwise carries the
             * negative half's ordering bias into the admission sum. The
             * threshold bias applies to every face, split or not. */
            double key_adj = -s_plane.thr_bias;
            if (s_plane.active && nemit) {
                int nvf = (f->i3 < s_sproj_n) ? 4 : 3;
                int neg = 0;
                for (int k = 0; k < nvf; k++) {
                    int vi = idx[k];
                    if (vi < s_sproj_n && s_sproj[vi].pd < 0.0) neg++;
                }
                if (neg == nvf) {
                    if (s_plane.drop_below) { s_plane_faces[2]++; continue; }
                    s_plane_faces[1]++;
                    key_adj += s_plane.key_bias;
                } else if (neg) s_plane_faces[3]++;
                else s_plane_faces[0]++;
            }
            if (nemit && player_game.face_depth_keep &&
                !player_game.face_depth_keep(cpu, s_xf[0].present ? &s_xf[0] : NULL,
                                             f, player_depth_sum(idx, order, nemit) + key_adj))
                continue;
            for (int e = 0; e < nemit; e++) {
                const int a = order[e][0], b = order[e][1], c = order[e][2];
                emit_swtri(idx[a], idx[b], idx[c], col[a], col[b], col[c],
                           f->u[a], f->v[a], f->u[b], f->v[b], f->u[c], f->v[c],
                           f->textured, f->tpx, f->tpy, f->clx, f->cly, f->bpp, blend);
            }
        }
    }
    s_eng_tris = s_swtri_n;
    return s_swtri_n;
}

/* ---- scene feed --------------------------------------------------------
 * Native float vertices carry the geometry; the face stream carries topology,
 * per-vertex colour and texture; the texture itself comes from VRAM, a static
 * asset. */

/** @brief The ordering-table slot the draw inserts a face's part at, plus one.
 *
 * The slot is max(0, (part-origin view-Z >> 7) - sortDepth), one slot per 128
 * full-world units (r_pete.s 80023F8C lbu 0x27 / sra 7 / sub / floor at 0;
 * spyrodraw.s 8004215C and 8003D148 read the same byte at 0x1E). The +1 keeps
 * 0 as "no key" (openpete_scene_tri::pkey). The face's part is the part its
 * first vertex was projected under; one face never spans two.
 *
 * The slot is an ordering key only: it never enters stored depth, which stays
 * true per vertex. Its one reader is the additive glow fan, which the table
 * paints under the avatar whenever his pulled slot is the nearer of the two.
 * @param vidx The face's first stream vertex index.
 * @param sd The sortDepth byte.
 * @return The slot + 1, or 0 when the index or its part has no transform. */
static float player_part_key(int vidx, int sd)
{
    if (vidx < 0 || vidx >= s_sproj_n) return 0.0f;
    int part = s_sproj[vidx].seg;
    if (part < 0 || part >= PLAYER_PARTS_MAX || !s_xf[part].present) return 0.0f;
    long slot = (long)floor(s_xf[part].TR[2]);
    slot = (slot >> 7) - sd;
    if (slot < 0) slot = 0;
    return (float)(slot + 1);
}
/** @brief The provenance tag of one walk triangle: its draw kind and the call's record slot.
 *
 * The mirrored call's triangles are all OPENPETE_TAG_PLAYER_MIRROR; on the
 * avatar call a triangle of the plane split's negative half is
 * OPENPETE_TAG_PLAYER_SUBMERGED and every other OPENPETE_TAG_PLAYER_AVATAR.
 * @param w The triangle.
 * @return The tag. */
static uint32_t player_tri_tag(const swtri* w) {
    const uint32_t kind = s_mirror_pass ? OPENPETE_TAG_PLAYER_MIRROR
                        : w->submerged  ? OPENPETE_TAG_PLAYER_SUBMERGED
                        : OPENPETE_TAG_PLAYER_AVATAR;
    return OPENPETE_TAG_PLAYER(kind, (uint32_t)s_player_slot);
}

/** @brief Print the submitted walk's screen-space centroid and bounding box when OPENPETE_PLAYER_POSTRACE is set.
 *
 * One line per call, for the record the feed just submitted. The centroid
 * follows the record's position and rotation brackets; the box also follows
 * the pose, so a centroid that glides across the sub-tick phases while the
 * box steps separates a translation defect from a keyframe-blend one. Prints
 * nothing for an empty walk. */
static void player_postrace(void) {
    static int s_pt = -1;
    ENV_FLAG_LATCH(s_pt, "OPENPETE_PLAYER_POSTRACE");
    if (!s_pt || s_swtri_n <= 0) return;
    extern int g_openpete_canon_lag_leg;
    double cx = 0.0, cy = 0.0;
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    int nv = 0;
    for (int i = 0; i < s_swtri_n; i++)
        for (int k = 0; k < 3; k++) {
            double sx = s_swtri[i].v[k].sx, sy = s_swtri[i].v[k].sy;
            cx += sx; cy += sy; nv++;
            if (sx < x0) x0 = sx;
            if (sx > x1) x1 = sx;
            if (sy < y0) y0 = sy;
            if (sy > y1) y1 = sy;
        }
    fprintf(stderr, "[player-screen f=%u] leg=%s slot=%d ntri=%d nv=%d "
                    "c=(%.3f,%.3f) bb=(%.3f,%.3f,%.3f,%.3f) mirror=%d\n",
            openpete_feed_key_now(), g_openpete_canon_lag_leg ? "CANON" : "inter",
            s_player_slot, s_swtri_n, nv, cx / nv, cy / nv, x0, y0, x1, y1,
            s_mirror_pass);
}

/** @brief Builds the face walk over the projection and adds its triangles to the scene.
 *
 * With `reproject` set the pose is projected first; otherwise the projection
 * already in s_sproj is used. Returns without touching the scene when the walk
 * built no triangle or the player is hidden (player_hide_active). The
 * sortDepth byte at PL.sort_depth_off selects the depth
 * policy: the provider's front sentinel pulls the whole model toward the near
 * plane by a cap shared across the tick's calls (player_flat_min), any other
 * value, the mirrored copy's negative one included, applies a one-slot tie
 * pull and enters only the ordering key; moby_player_sort_pull_on() off
 * (OPENPETE_PLAYER_SORT_PULL=0 or OPENPETE_MOBY_OTBIAS=0) disables both pulls.
 * Perspective W is taken from the unpulled view-Z. Each triangle carries the
 * PLAYER tag player_tri_tag gives it, and an opaque one an ordering key from
 * its first vertex's part. OPENPETE_SPYRO_UNTEX draws every triangle untextured;
 * OPENPETE_PLAYER_POSTRACE prints the submitted centroid and bounding box.
 * @param cpu Reads the pose and the sortDepth byte.
 * @param reproject Nonzero to run player_project_all first. */
static void player_feed_walk_ex(CPUState* cpu, int reproject) OP_REQUIRES(sim) {
    if (reproject) player_project_all(cpu);
    player_build_walk(cpu);
    if (s_swtri_n <= 0) return;
    const int hide = player_hide_active();
    player_hide_count(PH_CAT_PLAYER, s_swtri_n, hide);
    if (hide) return;

    /* The draw keys every part at max(0, (part-origin viewZ >> 7) -
     * sortDepth); one ordering slot is 128 full-world view-Z units. Like the
     * moby walks' depth offset, sortDepth is an ordering KEY, not geometry -
     * it never enters stored depth, and mirroring it as a world pull would
     * show the avatar through any wall within a constant distance. Per-pixel Z
     * over TRUE depth mirrors the per-face sort; the sub-slot tie pull below
     * resolves coincident surfaces to the avatar (ground contact, and a
     * carrier the rider sits inside).
     *
     * EXCEPTION: the saturated sentinel is the draw's own "ordering slot 0 -
     * draw in front of everything". That is authored full-front LAYERING, not
     * contact depth: keep the near-plane-capped whole-model pull for exactly
     * that value. OPENPETE_PLAYER_SORT_PULL=0 disables both. */
    double sp_pull = 0.0;
    double tie_pull = 0.0;
    int sd = PL.sort_depth_off ? (int)(int8_t)cpu->read_byte(PL.base + PL.sort_depth_off) : 0;
    {
        int pull_on = moby_player_sort_pull_on();
        if (pull_on && player_game.sort_depth_front && sd == player_game.sort_depth_front) {
            sp_pull = OPENPETE_OT_SLOT_WORLD * (double)sd;
            /* Cap the pull per tick so the nearest vertex lands ON the near
             * floor instead of clamping each vertex at it: a per-vertex clamp
             * flattens everything past the floor to ONE depth and kills
             * self-occlusion. */
            double minvz = 1e30;
            for (int i = 0; i < s_swtri_n; i++)
                for (int k = 0; k < 3; k++)
                    if (s_swtri[i].v[k].vz > 0.0f && (double)s_swtri[i].v[k].vz < minvz)
                        minvz = (double)s_swtri[i].v[k].vz;
            double fmin = player_flat_min(minvz);
            if (fmin - sp_pull < 1.0) sp_pull = fmin > 1.0 ? fmin - 1.0 : 0.0;
            if (minvz - sp_pull < 1.0) sp_pull = minvz > 1.0 ? minvz - 1.0 : 0.0;
        } else if (pull_on) {
            /* One-slot tie pull, on the avatar and the mirrored copy alike:
             * 127 world is under one ordering slot, so it can only flip pairs
             * the original called a tie. The mirrored copy's byte is negative
             * (-12 or -6): the draw keys it at (originZ >> 7) - sortDepth, so
             * the copy sorts that many slots behind the avatar. The byte stays
             * in the part key (player_part_key) and out of stored depth: the
             * copy at its true mirrored depth meets the reflected geometry
             * under the surface as the draw shows it, and a push of sortDepth
             * slots puts it behind a reflected wall the draw paints it over. */
            tie_pull = 127.0;
        }
    }
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_swtri_n; i++) {
        swtri* w = &s_swtri[i];
        openpete_native_scene_set_tag(player_tri_tag(w));
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = w->v[k].sx; st.v[k].sy = w->v[k].sy;
            double fvz = (double)w->v[k].vz - sp_pull - tie_pull;
            st.v[k].z  = openpete_native_scene_viewz(openpete_worldvz_full((float)fvz));
            /* Perspective W from the UNSHIFTED view-Z: the front-layer pull
             * puts the whole model on the near plane, and a W taken from that
             * depth varies across one model's faces and smears a texel over
             * them. */
            st.v[k].pvz = openpete_native_scene_viewz(openpete_worldvz_full(w->v[k].vz));
            st.v[k].u  = (float)w->v[k].u; st.v[k].v = (float)w->v[k].v;
            st.v[k].rgba = w->v[k].rgb | 0xFF000000u;
        }
        /* OPENPETE_SPYRO_UNTEX=1 draws the whole player feed untextured
         * (gouraud only), separating vertex-path losses from fragment-path
         * losses when bisecting a rendering defect. */
        { static int s_ut = -1;
          ENV_FLAG_LATCH(s_ut, "OPENPETE_SPYRO_UNTEX");
          if (s_ut) w->textured = 0; }
        st.textured = w->textured; st.tpage_x = w->tpage_x; st.tpage_y = w->tpage_y;
        st.clut_x = w->clut_x; st.clut_y = w->clut_y; st.bpp = w->bpp; st.blend = w->blend;
        st.pkey = w->blend == 0 ? player_part_key(w->v[0].idx, sd) : 0.0f;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_PLAYER, 0);
    }
    player_postrace();
    openpete_native_scene_set_tag(0);
}

/** @brief Projects the current pose and feeds its triangles to the scene.
 *
 * @param cpu Reads the pose. */
static void player_feed_walk(CPUState* cpu) OP_REQUIRES(sim) { player_feed_walk_ex(cpu, 1); }

/* ---- OPENPETE_PLAYER_VERIFY -------------------------------------------
 * The probe: bracket the draw's own polybuf window, decode the primitives it
 * emitted there, and match them against the triangles this walk built from the
 * same pose. Precision is the share of walk triangles a primitive covers,
 * recall the share of primitives a walk triangle covers, both on a 4-pixel
 * per-vertex bound; the mean is over the matched pairs' vertex distances and
 * the colour rate over their per-vertex packed RGB. The verify line carries the
 * parts, vertices, faces and triangles the walk read, so a run that decoded
 * nothing reports zero engagement instead of a vacuous pass.
 *
 * OPENPETE_PLAYER_VERIFY_BITSHIFT arms the negative control. */
#define PV_MAX_PRIM 4096
typedef struct { float x[3], y[3]; uint32_t rgb[3]; int u[3], v[3]; uint8_t code; } pv_tri;
static pv_tri s_pv[PV_MAX_PRIM]; static int s_pv_n;

/** @brief Decodes the polygon primitives of a polybuf range into s_pv as triangles.
 *
 * Walks the packet chain by each packet's length byte and keeps the polygon
 * codes 0x20-0x3F, flat or gouraud, triangle or quad, textured or not. A quad
 * becomes the triangles (0,1,2) and (1,3,2), the GPU's split. The walk stops at
 * `end`, at a zero or over-24-word length, or after PV_MAX_PRIM packets or
 * triangles; a packet shorter than its own layout is skipped.
 * @param cpu Reads the polybuf.
 * @param start The range's first packet.
 * @param end The range's end. */
static void pv_capture(CPUState* cpu, uint32_t start, uint32_t end) {
    s_pv_n = 0;
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < PV_MAX_PRIM) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t len = (w0 >> 24) & 0xFFu;
        if (len == 0 || len > 24) break;
        uint32_t stride = (len + 1) * 4;
        uint8_t code = (uint8_t)(cpu->read_word(p + 4) >> 24);
        if ((code & 0xE0u) == 0x20u) {
            int gouraud = (code & 0x10) != 0, quad = (code & 0x08) != 0, tex = (code & 0x04) != 0;
            int nv = quad ? 4 : 3;
            uint32_t raw[20];
            int nraw = (int)(stride / 4); if (nraw > 20) nraw = 20;
            for (int i = 0; i < nraw; i++) raw[i] = cpu->read_word(p + (uint32_t)i*4);
            float x[4], y[4]; uint32_t rgb[4]; int uu[4], vv[4];
            uint32_t rgb0 = raw[1] & 0xFFFFFFu;
            int c = 2, ok = 1;
            for (int k = 0; k < nv; k++) {
                rgb[k] = (gouraud && k > 0) ? (c < nraw ? raw[c++] & 0xFFFFFFu : rgb0) : rgb0;
                if (c >= nraw) { ok = 0; break; }
                uint32_t xy = raw[c++];
                x[k] = (float)(int16_t)(xy & 0xFFFF);
                y[k] = (float)(int16_t)(xy >> 16);
                uu[k] = vv[k] = -1;
                if (tex) {
                    if (c >= nraw) { ok = 0; break; }
                    uu[k] = (int)(raw[c] & 0xFFu);
                    vv[k] = (int)((raw[c] >> 8) & 0xFFu);
                    c++;
                }
            }
            if (ok) {
                const int split[2][3] = { {0,1,2}, {1,3,2} };   /* the GPU quad split */
                for (int sidx = 0; sidx < (quad ? 2 : 1) && s_pv_n < PV_MAX_PRIM; sidx++) {
                    pv_tri* t = &s_pv[s_pv_n++];
                    t->code = code;
                    for (int k = 0; k < 3; k++) {
                        int j = split[sidx][k];
                        t->x[k] = x[j]; t->y[k] = y[j]; t->rgb[k] = rgb[j];
                        t->u[k] = uu[j]; t->v[k] = vv[j];
                    }
                }
            }
        }
        p += stride;
    }
}

/** @brief Whether OPENPETE_PLAYER_VERIFY_MIRROR is set, latched on the first call.
 *
 * When set, the OPENPETE_PLAYER_VERIFY report runs on the mirrored copy's
 * call and its polybuf window instead of the avatar's, so the report's legs
 * measure the reflection.
 * @return 1 when set, else 0. */
static int player_verify_mirror(void) {
    static int v = -1;
    ENV_FLAG_LATCH(v, "OPENPETE_PLAYER_VERIFY_MIRROR");
    return v;
}

/** @brief Whether OPENPETE_PLAYER_VERIFY is set, latched on the first call.
 *
 * The first call also latches OPENPETE_PLAYER_VERIFY_BITSHIFT into
 * s_probe_bitshift, the probe's negative control.
 * @return 1 when set to a value not starting with 0, else 0. */
static int player_verify_on(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_PLAYER_VERIFY"); v = (e && *e && *e != '0');
        const char* b = getenv("OPENPETE_PLAYER_VERIFY_BITSHIFT");
        s_probe_bitshift = (b && *b && *b != '0');
    }
    return v;
}

/** @brief Scores the walk against the draw's own primitives and prints the probe lines.
 *
 * Captures the primitives of the polybuf window, then pairs walk triangles and
 * primitives one to one, cheapest first, within a 4-pixel per-vertex bound
 * under the best of three vertex rotations. Reports precision, recall, mean
 * vertex error and the colour, material and UV rates of the pairs, plus the
 * engagement counts of the walk. A per-index leg compares every projected
 * vertex with the draw's own screen word at PL.sxy_base; a plane leg, run while
 * the split is active or its negative control is set, compares the plane
 * distances with the draw's block at s_plane.dist_block, overall and per part.
 * Reads guest memory only.
 * @param cpu Reads the guest primitives and blocks.
 * @param startp The draw's polybuf start.
 * @param endp The draw's polybuf end. */
static void player_verify_report(CPUState* cpu, uint32_t startp, uint32_t endp) {
    pv_capture(cpu, startp, endp);
    /* One-to-one assignment, not nearest-neighbour: the avatar packs dozens of
     * sub-pixel triangles into a few pixels at range, and a nearest-centroid
     * match lets several walk triangles claim the same primitive while its
     * neighbours go unclaimed, which reads as a recall loss that is the
     * matcher's, not the walk's. Every pair within the 4-pixel per-vertex
     * bound is scored, the pairs are taken cheapest first, and each triangle
     * and each primitive is used once. */
    typedef struct { float cost; int16_t w, p; uint8_t rot; } pv_pair;
    static pv_pair pairs[PV_MAX_PRIM * 8];
    int npair = 0;
    for (int i = 0; i < s_swtri_n; i++) {
        const swtri* w = &s_swtri[i];
        for (int j = 0; j < s_pv_n; j++) {
            double br = 1e30; int rot = 0;
            for (int r = 0; r < 3; r++) {
                double mx = 0.0;
                for (int k = 0; k < 3; k++) {
                    double dx = w->v[k].sx - s_pv[j].x[(k + r) % 3];
                    double dy = w->v[k].sy - s_pv[j].y[(k + r) % 3];
                    double dd = sqrt(dx*dx + dy*dy);
                    if (dd > mx) mx = dd;
                }
                if (mx < br) { br = mx; rot = r; }
            }
            if (br <= 4.0 && npair < (int)(sizeof pairs / sizeof pairs[0])) {
                pairs[npair].cost = (float)br; pairs[npair].w = (int16_t)i;
                pairs[npair].p = (int16_t)j;   pairs[npair].rot = (uint8_t)rot;
                npair++;
            }
        }
    }
    for (int a = 1; a < npair; a++) {          /* insertion sort: npair is small */
        pv_pair t = pairs[a]; int b = a - 1;
        while (b >= 0 && pairs[b].cost > t.cost) { pairs[b+1] = pairs[b]; b--; }
        pairs[b+1] = t;
    }
    static uint8_t w_used[PV_MAX_PRIM * 8], p_used[PV_MAX_PRIM];
    memset(w_used, 0, sizeof w_used); memset(p_used, 0, sizeof p_used);
    long walk_hit = 0, prim_hit = 0, col_hit = 0, col_tot = 0, mat_hit = 0;
    long uv_hit = 0, uv_tot = 0; double uv_err = 0.0;
    double errsum = 0.0; long errn = 0;
    for (int a = 0; a < npair; a++) {
        int i = pairs[a].w, j = pairs[a].p, rot = pairs[a].rot;
        if (w_used[i] || p_used[j]) continue;
        w_used[i] = p_used[j] = 1;
        walk_hit++; prim_hit++;
        const swtri* w = &s_swtri[i];
        /* The material leg. A walk triangle can sit on its primitive to the
         * pixel and carry its exact vertex colours and still be the wrong
         * surface: the colour legs compare what modulates the texture, not
         * whether there is one. GPU primitive codes 0x20-0x3F set bit 2 for a
         * textured polygon, so the code the draw wrote is the draw's own
         * verdict on the material, and a walk that drops the texture reads as
         * a colour-perfect match without it. */
        if ((int)(w->textured ? 1 : 0) == (int)((s_pv[j].code & 0x04u) ? 1 : 0)) mat_hit++;
        for (int k = 0; k < 3; k++) {
            double dx = w->v[k].sx - s_pv[j].x[(k + rot) % 3];
            double dy = w->v[k].sy - s_pv[j].y[(k + rot) % 3];
            errsum += sqrt(dx*dx + dy*dy); errn++;
            col_tot++;
            if ((w->v[k].rgb & 0xFFFFFFu) == s_pv[j].rgb[(k + rot) % 3]) col_hit++;
            /* The UV leg. A matched, colour-exact, textured triangle still
             * samples the wrong texels when the walk's UV decode is off, and
             * neither the colour nor the material leg can see it: the texture
             * coordinates are compared exactly, over the matched pairs whose
             * primitive carries a pair at all. */
            int gu = s_pv[j].u[(k + rot) % 3], gv = s_pv[j].v[(k + rot) % 3];
            if (gu >= 0) {
                uv_tot++;
                if (w->v[k].u == gu && w->v[k].v == gv) uv_hit++;
                else uv_err += fabs((double)(w->v[k].u - gu)) + fabs((double)(w->v[k].v - gv));
            }
        }
    }
    /* The per-index leg: where the draw leaves every projected vertex's own
     * int16 screen position, index j of this walk is index j of that block, so
     * the comparison has no matching ambiguity and separates a decode error
     * from a culling difference. */
    long ix_hit = 0, ix_tot = 0; double ix_err = 0.0;
    if (PL.sxy_base) {
        for (int j = 0; j < s_sproj_n && (uint32_t)j * 4u < PL.sxy_len; j++) {
            uint32_t w = cpu->read_word(PL.sxy_base + (uint32_t)j * 4u);
            double gx = (double)(int16_t)(w & 0xFFFF), gy = (double)(int16_t)(w >> 16);
            double dx = s_sproj[j].sx - gx, dy = s_sproj[j].sy - gy;
            double d = sqrt(dx*dx + dy*dy);
            ix_tot++; ix_err += d; if (d <= 4.0) ix_hit++;
        }
    }
    /* The plane leg: where the split is armed the draw leaves its own signed
     * distance for every projected vertex, index for index with this walk, so
     * the comparison has no matching ambiguity and separates a wrong plane
     * from a wrong classification. */
    if (s_plane.active || player_plane_disabled()) {
        long pd_tot = 0, pd_sign = 0, pd_near = 0; double pd_err = 0.0;
        long pd_pt[PLAYER_PARTS_MAX] = {0}, pd_ps[PLAYER_PARTS_MAX] = {0};
        double pd_pe[PLAYER_PARTS_MAX] = {0};
        char pbuf[128]; int pn = 0;
        uint32_t db = s_plane.dist_block;
        if (db >= 0x80000000u) {
            for (int j = 0; j < s_sproj_n; j++) {
                int32_t g = (int32_t)cpu->read_word(db + (uint32_t)j * 4u);
                double d = s_sproj[j].pd;
                double e = fabs(d - (double)g);
                pd_tot++; pd_err += e;
                if (e <= 1.0) pd_near++;
                if ((g < 0) == (d < 0.0)) pd_sign++;
                int sg = s_sproj[j].seg;
                if (sg >= 0 && sg < PLAYER_PARTS_MAX) {
                    pd_pt[sg]++; pd_pe[sg] += e;
                    if ((g < 0) == (d < 0.0)) pd_ps[sg]++;
                }
            }
            /* Per part as well as overall: one part's record can be intact
             * while another's is not, and a whole-model average hides which. */
            for (int q = 0; q < PLAYER_PARTS_MAX && pn < (int)sizeof pbuf - 32; q++)
                if (pd_pt[q]) pn += snprintf(pbuf + pn, sizeof pbuf - (size_t)pn,
                                             " p%d=%.1f%%/%.1f(n=%ld)", q,
                                             100.0*(double)pd_ps[q]/pd_pt[q],
                                             pd_pe[q]/(double)pd_pt[q], pd_pt[q]);
        }
        pbuf[pn] = 0;
        fprintf(stderr,
            "[player-plane f=%u] active=%d drop=%d verts=%ld sign=%.1f%% <=1=%.1f%% "
            "mean=%.1f  faces above=%ld below=%ld dropped=%ld split=%ld%s%s\n",
            openpete_frame, s_plane.active, s_plane.drop_below, pd_tot,
            pd_tot ? 100.0*(double)pd_sign/pd_tot : 0.0,
            pd_tot ? 100.0*(double)pd_near/pd_tot : 0.0,
            pd_tot ? pd_err/(double)pd_tot : 0.0,
            s_plane_faces[0], s_plane_faces[1], s_plane_faces[2], s_plane_faces[3],
            pbuf, player_plane_disabled() ? "  [NEGATIVE CONTROL: plane refused]" : "");
    }
    fprintf(stderr,
        "[player-index f=%u] n=%ld <=4px=%.1f%% mean=%.2fpx\n",
        openpete_frame, ix_tot, ix_tot ? 100.0*(double)ix_hit/ix_tot : 0.0,
        ix_tot ? ix_err/(double)ix_tot : 0.0);
    fprintf(stderr,
        "[player-verify f=%u] parts=%ld verts=%ld faces=%ld tris=%ld prims=%d  "
        "precision=%.1f%% recall=%.1f%% mean=%.2fpx colour=%.1f%% material=%.1f%% "
        "uv=%.1f%%(n=%ld,err=%.2f)%s\n",
        openpete_frame, s_eng_parts, s_eng_verts, s_eng_faces, s_eng_tris, s_pv_n,
        s_swtri_n ? 100.0 * (double)walk_hit / s_swtri_n : 0.0,
        s_pv_n ? 100.0 * (double)prim_hit / s_pv_n : 0.0,
        errn ? errsum / (double)errn : 0.0,
        col_tot ? 100.0 * (double)col_hit / col_tot : 0.0,
        walk_hit ? 100.0 * (double)mat_hit / (double)walk_hit : 0.0,
        uv_tot ? 100.0 * (double)uv_hit / (double)uv_tot : 0.0, uv_tot,
        uv_tot ? uv_err / (double)uv_tot : 0.0,
        s_probe_bitshift ? "  [NEGATIVE CONTROL: model X one bit over]" : "");
}

/* ---- the hook ----------------------------------------------------------- */

/** @brief Runs the game's own avatar draw inside the hook bracket.
 *
 * @param cpu Passed to player_game.run_draw.
 * @param addr The hooked address; unused. */
static void player_run_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim); (void)addr; player_game.run_draw(cpu); }
/** @brief Whether the provider's own verify harness wants the hook live.
 *
 * @return The provider's answer, 0 when it has no harness. */
static int  player_verify_enabled(void) {
    OP_ROLE_ASSERT(sim);
    return player_game.verify_enabled ? player_game.verify_enabled() : 0;
}

static hook_bracket_state s_player_hb;
static const hook_bracket_desc k_player_bracket = {
    .path = RP_SPYRO, .prof_tag = "player.draw",
    .run = player_run_draw, .verify_enabled = player_verify_enabled,
    .extra_gate = player_verify_on,       /* the second verify mode, own report path */
    .st = &s_player_hb,
};

/** @brief The avatar draw hook: runs the game's draw once, records it, feeds the walk.
 *
 * Runs the draw inside the hook bracket, appends the call's model record when
 * the draw allocated polybuf, and latches the record's slot, the flatten group
 * and the mirror state. When the scene is live and the call is the avatar (or
 * the mirrored copy) with the player path selected, feeds the walk; for the
 * avatar it also snapshots the part block, colour table, SXY block and camera
 * into the sub-tick capture when one is open. The provider's verify and the
 * OPENPETE_PLAYER_VERIFY report run after the draw.
 * @param cpu The guest CPU state.
 * @param addr The hooked draw entry.
 * @return 0 when the bracket declines (inert or re-entered) so the ordinary
 *         dispatch runs the draw; 1 once the draw has run. */
static int on_player_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    if (!player_game.run_draw) return 0;
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_player_bracket, &startp, &endp, &feed, &verify))
        return 0;

    /* One record per call: the state bytes and scratch rows this call rendered
     * from. Its index is the tag slot of the triangles the walk emits. */
    int slot = 0;
    if (endp > startp) slot = player_records_model_append(cpu, startp, endp);
    player_native_set_player_slot(slot);
    player_native_set_flat_key(openpete_frame << 1);

    int is_player = player_game.call_is_player ? player_game.call_is_player(cpu) : 1;
    /* The avatar's ordering-table slot and table, for the 2D layer's
     * under-player order. */
    if (is_player && player_game.ot_slot) ui_sprite_player_link_note(player_game.ot_slot(cpu));
    /* The mirrored copy is a call of its own with its own record and tag slot:
     * latch it for the walk before anything reads the pose. */
    player_native_set_mirror(player_game.call_is_mirror
                             ? player_game.call_is_mirror(cpu) : 0);
    if (feed && (is_player || s_mirror_pass) && endp > startp && render_path_enabled(RP_SPYRO)) {
        player_feed_walk(cpu);
        /* Sub-tick spine: snapshot the part-record block and the projected SXY
         * (the cull verdict), both live now, so the intermediates can
         * re-derive the avatar. Where the records themselves are derived from
         * gamestate the block is still staged: a provider reads the words
         * around the records out of the same span (a per-part origin the
         * draw's later passes keep). */
        if (is_player && spyro_native_canon_capture_open()) {
            spyro_native_canon_capture_range(cpu, PL.part_off, PL.part_block_len);
            spyro_native_canon_capture_sxy(cpu);
            spyro_native_canon_capture_camera(cpu, SPYRO_PROD_SPYRO);
        }
    }

    if (verify && player_game.verify) player_game.verify(cpu, startp, endp);
    if (player_verify_on() && (player_verify_mirror() ? s_mirror_pass : is_player)
        && endp > startp) {
        player_project_all(cpu);
        player_build_walk(cpu);
        player_verify_report(cpu, startp, endp);
    }

    return hook_bracket_end(&k_player_bracket);
}

/* ---- sub-tick re-extract ------------------------------------------------ */

/* Set while a held camera cut replays tick N-1's records (player_records_set_hold),
 * and the store slot that present shows as a cut_held_pick_t, -1 outside a hold. */
static int s_pres_hold = 0;
static int s_pres_pick = -1;

void player_reextract_record(CPUState* cpu, int mirror) OP_REQUIRES(sim) {
    /* Which call of the tick this replay reproduces. The driver restores the
     * state word that told the two apart before the tick ends, so live RAM
     * cannot answer it here: the caller reads it off the record whose bytes
     * it staged. */
    player_native_set_mirror(mirror);
    /* Only re-extract on sub-ticks if the canonical tick actually drew the
     * avatar. A cutscene that suppresses the gameplay avatar and shows it as a
     * moby instead would otherwise get a SECOND avatar painted over the
     * cutscene one, at the same position in a different pose. The gate also
     * holds the native scene on and the path selected. A held cut replays tick
     * N-1's records, whose own drawn verdicts decide, so tick N's verdict does
     * not gate it. */
    if (!hook_reextract_ready_held(RP_SPYRO, s_pres_pick)) return;

    /* Sub-tick animation = two-pass keyframe-SHAPE interpolation: decode the
     * two REAL rendered poses, each internally consistent with its own seam,
     * and lerp the projected vertices. Rotation and position interpolate
     * through their own brackets, applied identically to both passes, so this
     * lerps only the SHAPE; the lerp of two real poses is always on-model. */
    int two_pass = 0;
    const uint32_t block_len = PL.part_block_len;
    if (s_anim_on && s_seg_valid >= 2 && block_len && block_len <= PLAYER_REC_PART_MAX) {
        /* A derived record needs no guest memory at all: the two rendered
         * poses are swapped through the walk's own copy, so the replay writes
         * nothing a present could leak into gameplay state. */
        if (player_game.part_record) {
            s_blk_pinned = 1;
            memcpy(s_blk, s_seg_prev, block_len);       /* pass 1: PREV pose */
            player_project_all(cpu);
            int pn = s_sproj_n;
            memcpy(s_sproj_prev, s_sproj, (size_t)pn * sizeof(s_sproj[0]));
            memcpy(s_blk, s_seg_curr, block_len);       /* pass 2: CURR pose */
            player_project_all(cpu);
            if (s_sproj_n == pn) {
                double a = s_anim_alpha;
                for (int i = 0; i < pn; i++) {
                    s_sproj[i].sx = (float)(s_sproj_prev[i].sx + (s_sproj[i].sx - s_sproj_prev[i].sx) * a);
                    s_sproj[i].sy = (float)(s_sproj_prev[i].sy + (s_sproj[i].sy - s_sproj_prev[i].sy) * a);
                    s_sproj[i].vz = (float)(s_sproj_prev[i].vz + (s_sproj[i].vz - s_sproj_prev[i].vz) * a);
                }
            }
            s_blk_pinned = 0;
            player_feed_walk_ex(cpu, 0);
            two_pass = 1;
        }
        uint32_t block = two_pass ? 0u : player_part_block_base(cpu);
        void* h = block ? openpete_subtick_host_span(block, block_len) : NULL;
        if (h) {
            uint8_t live[PLAYER_REC_PART_MAX];
            memcpy(live, h, block_len);                 /* the canonical (curr) block */
            memcpy(h, s_seg_prev, block_len);           /* pass 1: PREV rendered pose */
            player_project_all(cpu);
            int pn = s_sproj_n;
            memcpy(s_sproj_prev, s_sproj, (size_t)pn * sizeof(s_sproj[0]));
            memcpy(h, s_seg_curr, block_len);           /* pass 2: CURR rendered pose */
            player_project_all(cpu);
            if (s_sproj_n == pn) {                      /* index-matched -> lerp shape */
                double a = s_anim_alpha;
                for (int i = 0; i < pn; i++) {
                    s_sproj[i].sx = (float)(s_sproj_prev[i].sx + (s_sproj[i].sx - s_sproj_prev[i].sx) * a);
                    s_sproj[i].sy = (float)(s_sproj_prev[i].sy + (s_sproj[i].sy - s_sproj_prev[i].sy) * a);
                    s_sproj[i].vz = (float)(s_sproj_prev[i].vz + (s_sproj[i].vz - s_sproj_prev[i].vz) * a);
                }
            }
            memcpy(h, live, block_len);                 /* restore the live scratch */
            player_feed_walk_ex(cpu, 0);
            two_pass = 1;
        }
    }
    if (!two_pass) player_feed_walk(cpu);
}

void player_reextract(CPUState* cpu) OP_REQUIRES(sim) { player_reextract_record(cpu, 0); }

/* ======================================================================== *
 *  The per-call record store                                                *
 * ======================================================================== */

/* The auxiliary feeders, PLAYER_REC_SHADOW and PLAYER_REC_FLAME: auxiliary
 * index a is feeder PLAYER_REC_SHADOW + a. */
#define PLAYER_AUX_FEEDERS (PLAYER_REC_FEEDERS - PLAYER_REC_SHADOW)
#define AUX_OF(f) ((f) - PLAYER_REC_SHADOW)

typedef struct {
    int      n;
    uint32_t stamp;     /* canon epoch of the first append */
    uint32_t frame;     /* openpete_frame of the first append */
    player_model_rec_t r[PLAYER_REC_MAX];
    int      naux[PLAYER_AUX_FEEDERS];
    player_aux_rec_t aux[PLAYER_AUX_FEEDERS][PLAYER_REC_MAX];
} rec_list_t;

/** @brief Empties a list's model and auxiliary records.
 *
 * @param L The list. */
static void list_clear(rec_list_t* L) {
    L->n = 0;
    for (int a = 0; a < PLAYER_AUX_FEEDERS; a++) L->naux[a] = 0;
}

/** @brief Reports whether a list holds no model and no auxiliary record.
 *
 * @param L The list.
 * @return 1 when empty, else 0. */
static int list_empty(const rec_list_t* L) {
    if (L->n) return 0;
    for (int a = 0; a < PLAYER_AUX_FEEDERS; a++) if (L->naux[a]) return 0;
    return 1;
}

static rec_list_t s_list;             /* accumulating: the committed tick's hook calls */
static rec_list_t s_led;              /* accumulating: the speculative fires of one led frame */
static uint32_t   s_cap_refused_epoch, s_spec_refused_epoch, s_led_fires_epoch;

/** @brief Opens the committed append list for this call, emptying it when its key changed.
 *
 * The list is keyed by (epoch stamp, VBL): a non-empty list whose stamp or
 * VBL differs from the current one is emptied, and an empty list takes the
 * current key. The stamp advances only at a commit, which a fast-forward and a
 * CD stall skip, so a stamp-only key would accumulate a fast-forward's calls
 * to the cap. */
static void list_open(void) OP_REQUIRES(sim) {
    uint32_t stamp = spyro_native_canon_epoch_stamp();
    int empty = list_empty(&s_list);
    if (!empty && (s_list.stamp != stamp || s_list.frame != openpete_frame)) { list_clear(&s_list); empty = 1; }
    if (empty) { s_list.stamp = stamp; s_list.frame = openpete_frame; }
}

/** @brief Opens the led list for one speculative VBL's fires and counts the fire.
 *
 * Runahead runs speculative VBLs after the committed tick, and the last draw
 * among them is the ring's capture tick. The list is emptied when the VBL
 * count differs from the one it holds. */
static void led_open(void) {
    if (s_led.frame != openpete_frame) { list_clear(&s_led); s_led.frame = openpete_frame; }
    s_led_fires_epoch++;
}

typedef struct {
    rec_list_t list;
    op_time_t  ns;
    int        valid;
} store_slot_t;

static store_slot_t s_store[3];       /* [0] curr, [1] prev, [2] prev2 */
static uint32_t     s_store_epoch;
/* The store rolled at the last epoch commit: s_store[1] is then the list the
 * previous canonical epoch drew, else s_store[0] still is. */
static int          s_store_rolled;
static op_time_t    s_latched_ring_ns;
static uint32_t     s_reset_gen_seen;

/* Each feeder's guest record, plus the name of the interp-ring region that
 * captures it. The ring INDEX is resolved by name at first use: a restated
 * index selects the wrong region's bytes as soon as a row is inserted. */
static struct { const char* name; uint32_t addr; uint32_t len; const char* region; } s_reg[PLAYER_REC_FEEDERS];
static int s_reg_ready;

/** @brief Fills s_reg with each feeder's guest block, length and ring region name.
 *
 * Runs once; later calls return at once. */
static void reg_init(void) {
    if (s_reg_ready) return;
    s_reg_ready = 1;
    s_reg[PLAYER_REC_MODEL].name = "model";
    s_reg[PLAYER_REC_MODEL].addr = PL.base;
    s_reg[PLAYER_REC_MODEL].len  = PL.rec_len;
    s_reg[PLAYER_REC_MODEL].region = "g_Spyro";
    s_reg[PLAYER_REC_SHADOW].name = "shadow";
    s_reg[PLAYER_REC_SHADOW].addr = PL.shadow_base;
    s_reg[PLAYER_REC_SHADOW].len  = PL.shadow_len;
    s_reg[PLAYER_REC_SHADOW].region = "g_SpyroShadow";
    s_reg[PLAYER_REC_FLAME].name = "flame";
    s_reg[PLAYER_REC_FLAME].addr = PL.flame_base;
    s_reg[PLAYER_REC_FLAME].len  = PL.flame_len;
    s_reg[PLAYER_REC_FLAME].region = "g_SpyroFlame";
}

/** @brief The interp-ring region index of a feeder's state block, resolved once per feeder.
 *
 * @param f The feeder, PLAYER_REC_MODEL, PLAYER_REC_SHADOW or PLAYER_REC_FLAME.
 * @return The ring row index, or -1 when the layout gives the feeder no base
 *         address or the game registered no ring region by that name; a
 *         caller then has no ring bytes to compare against. */
static int reg_region(int f) {
    static int cache[PLAYER_REC_FEEDERS] = { -2, -2, -2 };
    reg_init();
    if (cache[f] == -2) cache[f] = s_reg[f].addr ? game_interp_row_index(s_reg[f].region) : -1;
    return cache[f];
}

#define REC_OBS_MAX   16

typedef struct {
    int      n, over;
    int32_t  pos[REC_OBS_MAX][3];
    uint8_t  cf[REC_OBS_MAX];
    uint8_t  last[PLAYER_REC_STATE_MAX];
    int      last_ok;
} feeder_obs_t;

static feeder_obs_t s_obs[PLAYER_REC_FEEDERS];
static feeder_obs_t s_prev_model;

static unsigned s_epochs, s_spec_refused, s_cap_refused, s_roll_ring, s_roll_records, s_roll_both, s_dedupe, s_resets;
static unsigned s_mismatch[PLAYER_REC_FEEDERS];
static unsigned s_no_ring, s_pairing_only, s_unpaired, s_pass2;
static unsigned s_led_fires, s_led_match, s_led_miss;
static int      s_max_calls[PLAYER_REC_FEEDERS];
static int      s_max_epochs_vbl, s_epochs_this_vbl;
static uint32_t s_last_commit_frame = 0xFFFFFFFFu;
static double   s_maxB_single = 0.0, s_maxB_multi = 0.0, s_minsep = -1.0, s_max_model_delta = 0.0;

int player_records_dbg(void) {
    static int on = -1;
    ENV_FLAG_LATCH(on, "OPENPETE_PLAYER_RECDBG");
    return on;
}

/** @brief The Euclidean distance between two int32 position triples. */
static double dist3(const int32_t a[3], const int32_t b[3]) {
    double dx = (double)a[0] - b[0], dy = (double)a[1] - b[1], dz = (double)a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}
/** @brief Copies a model record's position triple out of its state bytes.
 *
 * @param r The record.
 * @param out Receives the three words at PL.pos_off in host byte order. */
static void rec_pos(const player_model_rec_t* r, int32_t out[3]) { memcpy(out, r->spyro + PL.pos_off, 12); }

/** @brief Logs the live player position and copies a feeder's registered block, for the record census. */
static void observe(int feeder) {
    reg_init();
    feeder_obs_t* o = &s_obs[feeder];
    const uint8_t* sp = (const uint8_t*)openpete_psx_to_host_span(PL.base, PL.rec_len);
    const uint8_t* rb = s_reg[feeder].addr ? (const uint8_t*)openpete_psx_to_host_span(s_reg[feeder].addr, s_reg[feeder].len) : NULL;
    if (!sp || !rb) return;
    if (o->n < REC_OBS_MAX) { memcpy(o->pos[o->n], sp + PL.pos_off, 12); o->cf[o->n] = sp[0x2B]; o->n++; }
    else o->over++;
    memcpy(o->last, rb, s_reg[feeder].len);
    o->last_ok = 1;
}

int player_records_model_append(CPUState* cpu, uint32_t rp_start, uint32_t rp_end) OP_REQUIRES(sim) {
    /* Runahead lookahead: the hook fires on speculative ticks too. Those fires
     * are not the canonical epoch - they build the led list, the lerp source
     * of the committed records at commit. */
    rec_list_t* L = openpete_speculative ? &s_led : &s_list;
    if (openpete_speculative) led_open();
    else { if (player_records_dbg()) observe(PLAYER_REC_MODEL); list_open(); }
    if (L->n >= PLAYER_REC_MAX) { s_cap_refused_epoch++; return -1; }
    const uint8_t* sp  = (const uint8_t*)openpete_psx_to_host_span(PL.base, PL.rec_len);
    uint32_t block = player_part_block_base(cpu);
    const uint8_t* seg = block ? (const uint8_t*)openpete_psx_to_host_span(block, PL.part_block_len) : NULL;
    if (player_game.part_record) seg = s_blk;   /* set below, from gamestate */
    const uint8_t* sxy = PL.sxy_base ? (const uint8_t*)openpete_psx_to_host_span(PL.sxy_base, PL.sxy_len) : NULL;
    if (!sp || !seg) return -1;
    player_model_rec_t* r = &L->r[L->n];
    memset(r, 0, sizeof *r);
    memcpy(r->spyro, sp,  PL.rec_len);
    memcpy(r->src,   sp,  PL.rec_len);
    if (player_game.part_record) player_block_derive(cpu, r->seg);
    else memcpy(r->seg, seg, PL.part_block_len);
    if (sxy) memcpy(r->sxy, sxy, PL.sxy_len);
    r->rp_start = rp_start; r->rp_end = rp_end;
    r->drawn = 1; r->pair = -1; r->pair2 = -1;
    r->is_player = (uint8_t)(player_game.call_is_player ? (player_game.call_is_player(cpu) ? 1 : 0) : 1);
    return L->n++;
}

/** @brief Appends a pairing-only model record copied from a state block.
 *
 * The record is zeroed, takes the state as both its key and its shown
 * state, and is unpaired, undrawn and the player's; the caller checks the
 * cap.
 * @param L The list.
 * @param state The player state block, PL.rec_len bytes.
 * @return The record's index. */
static int pairing_model_push(rec_list_t* L, const uint8_t* state) {
    player_model_rec_t* r = &L->r[L->n];
    memset(r, 0, sizeof *r);
    memcpy(r->spyro, state, PL.rec_len);
    memcpy(r->src,   state, PL.rec_len);
    r->drawn = 0; r->pair = -1; r->pair2 = -1; r->is_player = 1;
    return L->n++;
}

/** @brief The model record an auxiliary record renders under.
 *
 * The newest model record whose position words equal the live ones is
 * returned, since a caller draws model, shadow and flame per avatar. When none
 * matches (hidden, respawn blink), a pairing-only model record copied from the
 * live state is appended: the next drawn model pairs with it and the shadow or
 * flame renders under its state fields; it is never rendered itself.
 * @param L The list being built.
 * @param sp Host pointer to the live player record.
 * @return The model record's index, or -1 at the cap. */
static int bind_model(rec_list_t* L, const uint8_t* sp) {
    for (int k = L->n - 1; k >= 0; k--)
        if (memcmp(L->r[k].spyro + PL.pos_off, sp + PL.pos_off, 12) == 0) return k;
    if (L->n >= PLAYER_REC_MAX) { s_cap_refused_epoch++; return -1; }
    if (L == &s_list) s_pairing_only++;
    return pairing_model_push(L, sp);
}

/** @brief Appends an auxiliary record of feeder index a to a list.
 *
 * The state and shown-state copies both take `state`; the caller checks the
 * cap.
 * @param L The list.
 * @param a The auxiliary index.
 * @param state The feeder's state block.
 * @param model The model record it renders under.
 * @param drawn 1 for a hook call, 0 for pairing-only. */
static void aux_push(rec_list_t* L, int a, const uint8_t* state, int model, int drawn) {
    const uint32_t len = s_reg[PLAYER_REC_SHADOW + a].len;
    player_aux_rec_t* d = &L->aux[a][L->naux[a]++];
    memcpy(d->st,  state, len);
    memcpy(d->src, state, len);
    d->model = (int8_t)model; d->drawn = (uint8_t)drawn;
}

/** @brief Records one call of an auxiliary feeder's hook, bound to the model record it renders under.
 *
 * A speculative call goes to the led list, a committed one to the epoch
 * list.
 * @param f The feeder, PLAYER_REC_SHADOW or PLAYER_REC_FLAME.
 * @return The model record's index, or -1 when the layout has no block for
 *         the feeder, a list is at the cap, or a block does not map. */
static int aux_append(int f) OP_REQUIRES(sim) {
    reg_init();
    const int a = AUX_OF(f);
    if (!s_reg[f].addr) return -1;
    rec_list_t* L = openpete_speculative ? &s_led : &s_list;
    if (openpete_speculative) led_open();
    else { if (player_records_dbg()) observe(f); list_open(); }
    if (L->naux[a] >= PLAYER_REC_MAX) { s_cap_refused_epoch++; return -1; }
    const uint8_t* sp = (const uint8_t*)openpete_psx_to_host_span(PL.base, PL.rec_len);
    const uint8_t* st = (const uint8_t*)openpete_psx_to_host_span(s_reg[f].addr, s_reg[f].len);
    if (!sp || !st) return -1;
    int m = bind_model(L, sp);
    if (m < 0) return -1;
    aux_push(L, a, st, m, 1);
    return m;
}

int player_records_shadow_append(CPUState* cpu) OP_REQUIRES(sim) { (void)cpu; return aux_append(PLAYER_REC_SHADOW); }

int player_records_flame_append(CPUState* cpu) OP_REQUIRES(sim) { (void)cpu; return aux_append(PLAYER_REC_FLAME); }

/** @brief Whether two record lists differ in any byte the store compares.
 *
 * Compares the model, shadow and flame counts, and each model record's drawn
 * flag and state bytes; a drawn record's part block and SXY block
 * as well; each shadow and flame record's model index, drawn flag and state.
 * The shown-state copies, polybuf ranges and pairing fields are not compared.
 * @param a The first list.
 * @param b The second list.
 * @return 1 when they differ, else 0. */
static int lists_differ(const rec_list_t* a, const rec_list_t* b) {
    if (a->n != b->n) return 1;
    for (int k = 0; k < a->n; k++) {
        const player_model_rec_t *x = &a->r[k], *y = &b->r[k];
        if (x->drawn != y->drawn) return 1;
        if (memcmp(x->spyro, y->spyro, PL.rec_len) != 0) return 1;
        if (x->drawn && (memcmp(x->seg, y->seg, PL.part_block_len) != 0 ||
                         memcmp(x->sxy, y->sxy, PL.sxy_len) != 0)) return 1;
    }
    reg_init();
    for (int f = 0; f < PLAYER_AUX_FEEDERS; f++) {
        const uint32_t len = s_reg[PLAYER_REC_SHADOW + f].len;
        if (a->naux[f] != b->naux[f]) return 1;
        for (int j = 0; j < a->naux[f]; j++) {
            const player_aux_rec_t *x = &a->aux[f][j], *y = &b->aux[f][j];
            if (x->model != y->model || x->drawn != y->drawn || memcmp(x->st, y->st, len) != 0) return 1;
        }
    }
    return 0;
}

/** @brief Pairs each model record of `cur` with a record of `prv`, and each with the record before that through the previous pairing.
 *
 * With no previous list every record stays unpaired. One record on each side
 * pairs unconditionally. Otherwise two passes run: index first, kept when the
 * positions lie within pair_bound; then the nearest still-unpaired previous
 * record within the bound, ties to the lower index. pair2 is the paired
 * record's own pair. Runs on the host loop's thread and reads no clock.
 * @param cur The newest list; its pair and pair2 fields are rewritten.
 * @param prv The previous list, or NULL. */
static void pair_lists(rec_list_t* cur, const rec_list_t* prv) {
    int taken[PLAYER_REC_MAX] = {0};
    const double B = PL.pair_bound;
    for (int k = 0; k < cur->n; k++) { cur->r[k].pair = -1; cur->r[k].pair2 = -1; }
    if (!prv) { s_unpaired += (unsigned)cur->n; return; }
    /* One record each side: nothing to disambiguate, so they pair
     * unconditionally, as the ring's prev/curr do (the camera cut
     * guard, not the bound, decides a teleport there). */
    if (cur->n == 1 && prv->n == 1) { cur->r[0].pair = 0; cur->r[0].pair2 = prv->r[0].pair; return; }
    for (int k = 0; k < cur->n && k < prv->n; k++) {
        int32_t a[3], b[3]; rec_pos(&cur->r[k], a); rec_pos(&prv->r[k], b);
        if (dist3(a, b) <= B) { cur->r[k].pair = (int8_t)k; taken[k] = 1; }
    }
    for (int k = 0; k < cur->n; k++) {
        if (cur->r[k].pair >= 0) continue;
        int32_t a[3]; rec_pos(&cur->r[k], a);
        int best = -1; double bd = 0.0;
        for (int j = 0; j < prv->n; j++) {
            if (taken[j]) continue;
            int32_t b[3]; rec_pos(&prv->r[j], b);
            double d = dist3(a, b);
            if (d <= B && (best < 0 || d < bd)) { best = j; bd = d; }
        }
        if (best >= 0) { cur->r[k].pair = (int8_t)best; taken[best] = 1; s_pass2++; }
    }
    for (int k = 0; k < cur->n; k++) {
        if (cur->r[k].pair >= 0) cur->r[k].pair2 = prv->r[cur->r[k].pair].pair;
        else s_unpaired++;
    }
}

/** @brief Prints the record census counters as one stderr line.
 *
 * Registered with atexit by player_records_epoch_commit under
 * OPENPETE_PLAYER_RECDBG; the line starts with [recdbg-summary]. */
static void summary(void) {
    OP_ROLE_ASSERT(sim);
    fprintf(stderr,
        "[recdbg-summary] epochs=%u no_ring=%u max_calls m=%d s=%d f=%d spec_refused=%u cap_refused=%u "
        "max_epochs_per_vbl=%d roll_ring=%u roll_records=%u roll_both=%u dedupe=%u resets=%u "
        "pairing_only=%u unpaired=%u pass2=%u led_fires=%u led_match=%u led_miss=%u "
        "mismatch m=%u s=%u f=%u max_model_pos_delta=%.0f "
        "maxB n=1:%.0f n>=2:%.0f minsep=%.0f\n",
        s_epochs, s_no_ring, s_max_calls[0], s_max_calls[1], s_max_calls[2], s_spec_refused, s_cap_refused,
        s_max_epochs_vbl, s_roll_ring, s_roll_records, s_roll_both, s_dedupe, s_resets,
        s_pairing_only, s_unpaired, s_pass2, s_led_fires, s_led_match, s_led_miss,
        s_mismatch[0], s_mismatch[1], s_mismatch[2], s_max_model_delta,
        s_maxB_single, s_maxB_multi, s_minsep);
}

/** @brief The ring snapshot's bytes of a feeder's registered region, or NULL when the ring has no row for it.
 *
 * @param curr The ring's current snapshot.
 * @param f The feeder.
 * @return The host bytes. */
static const uint8_t* ring_block(const openpete_interp_snapshot_t* curr, int f) {
    const int fr = reg_region(f);
    return fr >= 0 ? curr->data + curr->region_offsets[fr] : NULL;
}

/** @brief Invalidates the store and the latched ring stamp when the interp ring reset.
 *
 * A savestate load, rewind or runahead depth switch steps the ring's reset
 * generation; the store's history then describes the pre-jump timeline. */
static void epoch_reset_on_ring_reset(void) {
    uint32_t gen = openpete_interp_reset_generation();
    if (gen == s_reset_gen_seen) return;
    s_reset_gen_seen = gen;
    for (int i = 0; i < 3; i++) s_store[i].valid = 0;
    s_latched_ring_ns = 0;
    s_resets++;
}

/** @brief Counts the commit against its VBL and keeps the most commits seen in one VBL. */
static void epoch_count_vbl(void) {
    if (openpete_frame == s_last_commit_frame) s_epochs_this_vbl++;
    else { s_epochs_this_vbl = 1; s_last_commit_frame = openpete_frame; }
    if (s_epochs_this_vbl > s_max_epochs_vbl) s_max_epochs_vbl = s_epochs_this_vbl;
}

/** @brief Takes each record's shown state from the led list under --runahead.
 *
 * The hooks fire on the committed tick and on the speculative ticks; the
 * ring and the canonical present hold the led one. Every model record takes
 * src (and, drawn, seg and sxy) from the led list index for index when the
 * led list is of a later VBL no later than the ring's tick, of the same
 * length and the same drawn flags; each auxiliary feeder's records take
 * src when its counts match. The committed bytes stay the pairing key. A
 * mismatched led list leaves the records committed and is counted.
 * @param epoch The committed list.
 * @param curr The ring's current snapshot. */
static void epoch_take_led(rec_list_t* epoch, const openpete_interp_snapshot_t* curr) {
    int led_ok = (s_led.frame > epoch->frame &&
                  s_led.frame <= curr->tick && s_led.n == epoch->n);
    for (int k = 0; led_ok && k < epoch->n; k++)
        if (s_led.r[k].drawn != epoch->r[k].drawn) led_ok = 0;
    if (!led_ok) { s_led_miss++; return; }
    for (int k = 0; k < epoch->n; k++) {
        memcpy(epoch->r[k].src, s_led.r[k].spyro, PL.rec_len);
        if (epoch->r[k].drawn) {
            memcpy(epoch->r[k].seg, s_led.r[k].seg, PL.part_block_len);
            memcpy(epoch->r[k].sxy, s_led.r[k].sxy, PL.sxy_len);
        }
    }
    for (int a = 0; a < PLAYER_AUX_FEEDERS; a++) {
        if (s_led.naux[a] != epoch->naux[a]) continue;
        for (int j = 0; j < epoch->naux[a]; j++)
            memcpy(epoch->aux[a][j].src, s_led.aux[a][j].st, s_reg[PLAYER_REC_SHADOW + a].len);
    }
    s_led_match++;
}

/** @brief Appends pairing-only auxiliary records for the ring's avatar whose hooks did not fire.
 *
 * The ring's avatar is the newest model record whose shown position equals
 * the ring's. Each auxiliary feeder with a ring row and no record bound to
 * it gets one from the ring's tick-end bytes (the anchor and the ribbon
 * state are written every tick, drawn or not), so a record that appears
 * next tick lerps from them. Never rendered.
 * @param epoch The committed list.
 * @param curr The ring's current snapshot.
 * @param ring The ring's model bytes. */
static void epoch_add_pairing_aux(rec_list_t* epoch, const openpete_interp_snapshot_t* curr,
                                  const uint8_t* ring) {
    int rk = -1;
    for (int k = epoch->n - 1; k >= 0; k--)
        if (memcmp(epoch->r[k].src + PL.pos_off, ring + PL.pos_off, 12) == 0) { rk = k; break; }
    if (rk < 0) return;
    for (int a = 0; a < PLAYER_AUX_FEEDERS; a++) {
        int has = 0;
        for (int j = 0; j < epoch->naux[a]; j++) if (epoch->aux[a][j].model == rk) { has = 1; break; }
        const uint8_t* rb = ring_block(curr, PLAYER_REC_SHADOW + a);
        if (!has && epoch->naux[a] < PLAYER_REC_MAX && rb) aux_push(epoch, a, rb, rk, 0);
    }
}

/** @brief Adds the pairing-only records the ring's tick needs to the committed list.
 *
 * A model record is appended from the ring's bytes when no hook observed the
 * ring's tick-end position (the avatar was not drawn this tick, or the
 * epoch had no call); under --runahead with led fires the led list supplies
 * the shown state instead (epoch_take_led) and no model record is added.
 * Then the ring's avatar's missing auxiliary records are added
 * (epoch_add_pairing_aux).
 * @param epoch The committed list.
 * @param curr The ring's current snapshot. */
static void epoch_add_pairing(rec_list_t* epoch, const openpete_interp_snapshot_t* curr) {
    const uint8_t* ring = ring_block(curr, PLAYER_REC_MODEL);
    if (!ring) return;
    int seen = 0;
    for (int k = 0; k < epoch->n; k++)
        if (memcmp(epoch->r[k].spyro + PL.pos_off, ring + PL.pos_off, 12) == 0) { seen = 1; break; }
    if (openpete_runahead_get() > 0 && epoch->n > 0 && s_led_fires_epoch > 0) {
        epoch_take_led(epoch, curr);
        seen = 1;
    }
    if (!seen && epoch->n < PLAYER_REC_MAX) {
        pairing_model_push(epoch, ring);
        s_pairing_only++;
    }
    epoch_add_pairing_aux(epoch, curr, ring);
}

/** @brief Rolls the committed list into the store, or drops it as a duplicate.
 *
 * The list becomes the newest store entry, paired against the previous one,
 * when the ring rolled or the list differs from the newest. Its clock is
 * the ring's stamp when the ring rolled; a record-only roll stamps the
 * store's own grid (previous + one period) under deterministic capture,
 * else the tick stamp (the VBL under the PLL), or `ns` when that is 0.
 * @param epoch The committed list.
 * @param ring_rolled Nonzero when the ring's tick stamp changed.
 * @param ns The ring's current tick stamp.
 * @return Nonzero when the list differs from the newest store entry. */
static int store_roll(const rec_list_t* epoch, int ring_rolled, op_time_t ns) {
    int rec_changed = !s_store[0].valid || lists_differ(epoch, &s_store[0].list);
    if (ring_rolled && rec_changed) s_roll_both++;
    else if (ring_rolled)           s_roll_ring++;
    else if (rec_changed)           s_roll_records++;
    else                            s_dedupe++;
    s_store_rolled = 0;
    if (!ring_rolled && !rec_changed) return rec_changed;
    s_store_rolled = 1;
    s_store[2] = s_store[1];
    s_store[1] = s_store[0];
    s_store[0].list = *epoch;
    s_store[0].valid = 1;
    if (ring_rolled) s_store[0].ns = ns;
    else if (openpete_interp_deterministic() && s_store[1].valid)
        s_store[0].ns = s_store[1].ns + openpete_interp_det_period();
    else {
        const op_time_t stamp = atomic_load_explicit(&g_openpete_tick_stamp, memory_order_relaxed);
        s_store[0].ns = stamp ? stamp : ns;
    }
    pair_lists(&s_store[0].list, s_store[1].valid ? &s_store[1].list : NULL);
    return rec_changed;
}

/** @brief Compares each feeder's last observed block with the ring's and folds the mismatches into the census.
 *
 * @param curr The ring's current snapshot, or NULL.
 * @param mm Receives 1 per feeder whose block differs from the ring's.
 * @return The model's position delta to the ring on a model mismatch, else 0. */
static double recdbg_ring_mismatch(const openpete_interp_snapshot_t* curr, int mm[PLAYER_REC_FEEDERS]) {
    double model_delta = 0.0;
    for (int f = 0; f < PLAYER_REC_FEEDERS; f++) {
        mm[f] = 0;
        if (!curr || !s_obs[f].last_ok) continue;
        const uint8_t* ring = ring_block(curr, f);
        if (!ring) continue;
        mm[f] = memcmp(s_obs[f].last, ring, s_reg[f].len) != 0;
        if (mm[f]) s_mismatch[f]++;
        if (f == PLAYER_REC_MODEL && mm[f]) {
            int32_t rp[3]; memcpy(rp, ring + PL.pos_off, 12);
            model_delta = dist3(s_obs[f].pos[s_obs[f].n - 1], rp);
            if (model_delta > s_max_model_delta) s_max_model_delta = model_delta;
        }
    }
    return model_delta;
}

/** @brief The largest move of an observed model position from the previous epoch's same-index one, folded into the census maximum.
 *
 * Kept per single and multiple records.
 * @param m This epoch's model observations.
 * @return The move, 0 when the counts differ or nothing was observed. */
static double recdbg_model_move(const feeder_obs_t* m) {
    double B = 0.0;
    if (m->n <= 0 || m->n != s_prev_model.n) return B;
    for (int k = 0; k < m->n; k++) { double d = dist3(m->pos[k], s_prev_model.pos[k]); if (d > B) B = d; }
    double* maxB = m->n == 1 ? &s_maxB_single : &s_maxB_multi;
    if (B > *maxB) *maxB = B;
    return B;
}

/** @brief The smallest distance between two of this epoch's observed model positions, folded into the census minimum.
 *
 * @param m This epoch's model observations.
 * @return The separation, -1 with fewer than two positions. */
static double recdbg_model_sep(const feeder_obs_t* m) {
    double sep = -1.0;
    for (int i = 0; i < m->n; i++)
        for (int j = i + 1; j < m->n; j++) { double d = dist3(m->pos[i], m->pos[j]); if (sep < 0.0 || d < sep) sep = d; }
    if (sep >= 0.0 && (s_minsep < 0.0 || sep < s_minsep)) s_minsep = sep;
    return sep;
}

/** @brief Prints one [recdbg] line for the commit: calls, ring agreement, bounds, store pairs and bindings.
 *
 * @param curr The ring's current snapshot, or NULL.
 * @param ring_rolled Nonzero when the ring's tick stamp changed.
 * @param rec_changed Nonzero when the list differed from the newest store entry. */
static void recdbg_epoch_line(const openpete_interp_snapshot_t* curr, int ring_rolled, int rec_changed) {
    static const char* const aux_label[PLAYER_AUX_FEEDERS] = { "shadows", "flames" };
    const feeder_obs_t* m = &s_obs[PLAYER_REC_MODEL];
    int mm[PLAYER_REC_FEEDERS];
    double model_delta = recdbg_ring_mismatch(curr, mm);
    const double B = recdbg_model_move(m), sep = recdbg_model_sep(m);
    for (int f = 0; f < PLAYER_REC_FEEDERS; f++)
        if (s_obs[f].n + s_obs[f].over > s_max_calls[f]) s_max_calls[f] = s_obs[f].n + s_obs[f].over;
    const rec_list_t* L = &s_store[0].list;
    fprintf(stderr, "[recdbg f=%u e=%u] calls m=%d s=%d f=%d spec=%u led=%d@%u cap=%u epochs_vbl=%d ring=%d recchg=%d "
            "mm=%d/%d/%d mdelta=%.0f B=%.0f sep=%.0f store=%d pos=",
            openpete_feed_key_now(), s_epochs, m->n + m->over, s_obs[1].n + s_obs[1].over, s_obs[2].n + s_obs[2].over,
            s_spec_refused_epoch, s_led_fires_epoch ? s_led.n : 0, s_led_fires_epoch ? s_led.frame : 0u,
            s_cap_refused_epoch, s_epochs_this_vbl, ring_rolled, rec_changed,
            mm[0], mm[1], mm[2], model_delta, B, sep, L->n);
    for (int k = 0; k < m->n; k++)
        fprintf(stderr, "%s(%d,%d,%d)c%u", k ? ";" : "", m->pos[k][0], m->pos[k][1], m->pos[k][2], m->cf[k]);
    fprintf(stderr, " pairs=");
    for (int k = 0; k < L->n; k++)
        fprintf(stderr, "%s%d%s", k ? "," : "", L->r[k].pair, L->r[k].drawn ? "" : "p");
    for (int a = 0; a < PLAYER_AUX_FEEDERS; a++) {
        fprintf(stderr, " %s=", aux_label[a]);
        for (int j = 0; j < L->naux[a]; j++)
            fprintf(stderr, "%s%d%s", j ? "," : "", L->aux[a][j].model, L->aux[a][j].drawn ? "" : "p");
    }
    fputc('\n', stderr);
    s_prev_model = *m;
}

void player_records_epoch_commit(void) OP_REQUIRES(sim) {
    if (!PL.base) return;
    reg_init();
    const int dbg = player_records_dbg();
    static int armed = 0;
    if (dbg && !armed) { armed = 1; atexit(summary); }
    s_epochs++;
    epoch_reset_on_ring_reset();
    epoch_count_vbl();

    static rec_list_t epoch;
    if (!list_empty(&s_list) && spyro_native_canon_epoch_fresh(s_list.stamp)) epoch = s_list;
    else list_clear(&epoch);
    list_clear(&s_list);

    const openpete_interp_snapshot_t* curr = openpete_interp_curr_snapshot();
    op_time_t ns = openpete_interp_curr_tick_ns();
    int ring_rolled = (ns != 0 && ns != s_latched_ring_ns);
    s_latched_ring_ns = ns;
    if (!curr) s_no_ring++;
    if (curr) epoch_add_pairing(&epoch, curr);

    int rec_changed = store_roll(&epoch, ring_rolled, ns);
    s_store_epoch = spyro_native_canon_epoch_stamp() - 1u;

    s_led_fires += s_led_fires_epoch;
    s_spec_refused += s_spec_refused_epoch;
    s_cap_refused  += s_cap_refused_epoch;
    if (dbg) recdbg_epoch_line(curr, ring_rolled, rec_changed);
    memset(s_obs, 0, sizeof s_obs);
    s_spec_refused_epoch = 0;
    s_led_fires_epoch = 0;
    s_cap_refused_epoch = 0;
}

/* ---- present side -------------------------------------------------------- */

static int    s_pres_n = 0;
static int    s_pres_b = 0;   /* the store slot the present shows as its newest */

/** @brief The store slot k entries older than the one the present shows, or NULL past the store or on an empty slot.
 *
 * @param k 0 for the shown slot, 1 for its previous, 2 for the one before.
 * @return The slot, or NULL. */
static const store_slot_t* pres_slot(int k) {
    const int i = s_pres_b + k;
    return (i < 3 && s_store[i].valid) ? &s_store[i] : NULL;
}

void player_records_set_hold(int on) { s_pres_hold = on ? 1 : 0; s_pres_pick = -1; }
int  player_records_held_pick(void) { return s_pres_pick; }
static int    s_pres_lerp = 0;
static double s_pres_alpha = 0.0;

/** @brief Shows tick N-1's records whole on a held cut: its own set, drawn verdicts and state, with nothing to lerp toward.
 *
 * The store's newest slot is tick N-1's when the store did not roll at the
 * last commit, else the slot before it; s_pres_pick records which.
 * @return The record count shown, 0 when tick N-1's slot is empty. */
static int pres_begin_held(void) {
    s_pres_pick = CUT_HELD_CURR;
    if (s_store_rolled) {
        if (!s_store[1].valid) { s_pres_pick = CUT_HELD_EMPTY; return 0; }
        s_pres_b = 1;
        s_pres_pick = CUT_HELD_PREV;
    }
    s_pres_n = s_store[s_pres_b].list.n;
    cut_census_held_set("player", (cut_held_pick_t)s_pres_pick, s_store_epoch - 1u, s_pres_n);
    return s_pres_n;
}

int player_records_present_begin(int ring_paired, int ring_cut, double ring_alpha, double* out_alpha) {
    s_pres_n = 0; s_pres_lerp = 0; s_pres_alpha = 0.0; s_pres_b = 0;
    if (out_alpha) *out_alpha = 0.0;
    if (!s_store[0].valid) return 0;
    if (!spyro_native_canon_epoch_fresh(s_store_epoch)) return 0;
    if (s_pres_hold) return pres_begin_held();
    s_pres_n = s_store[0].list.n;
    if (ring_paired && !ring_cut && s_store[1].valid) {
        s_pres_lerp = 1;
        if (s_store[0].ns == openpete_interp_curr_tick_ns()) {
            /* The store rolled with the ring: one clock, the ring's alpha. */
            s_pres_alpha = ring_alpha;
        } else {
            op_time_t p = s_store[0].ns - s_store[1].ns;
            if (p > g_openpete_tick_period_ns) p = g_openpete_tick_period_ns;
            double a = (p > 0) ? (double)((int64_t)openpete_interp_present_now() - (int64_t)s_store[0].ns) / (double)p : 1.0;
            if (a < -0.35) a = -0.35; else if (a > 1.0) a = 1.0;
            s_pres_alpha = a;
        }
    }
    if (out_alpha) *out_alpha = s_pres_alpha;
    return s_pres_n;
}

const player_model_rec_t* player_records_curr(int k) {
    return (k >= 0 && k < s_pres_n) ? &s_store[s_pres_b].list.r[k] : NULL;
}
const player_model_rec_t* player_records_prev(int k) {
    const player_model_rec_t* c = player_records_curr(k);
    const store_slot_t* p = pres_slot(1);
    if (!c || c->pair < 0 || !p || c->pair >= p->list.n) return NULL;
    return &p->list.r[c->pair];
}
const player_model_rec_t* player_records_prev2(int k) {
    const player_model_rec_t* c = player_records_curr(k);
    const store_slot_t* p = pres_slot(2);
    if (!c || c->pair2 < 0 || !p || c->pair2 >= p->list.n) return NULL;
    return &p->list.r[c->pair2];
}
int player_records_lerps(int k) {
    const player_model_rec_t* c = player_records_curr(k);
    return c && s_pres_lerp && c->pair >= 0;
}
int player_records_model_drawn(int k) {
    const rec_list_t* L = openpete_speculative ? &s_led : &s_list;
    return (k >= 0 && k < L->n && L->r[k].drawn) ? 1 : 0;
}

/** @brief The auxiliary record count of the open present.
 *
 * @param a The auxiliary index.
 * @return The count, 0 when no present is open. */
static int aux_count(int a) { return s_pres_n ? s_store[s_pres_b].list.naux[a] : 0; }

/** @brief The newest store entry's auxiliary record j of the open present.
 *
 * @param a The auxiliary index.
 * @param j The record index.
 * @return The record, or NULL when `j` is out of range or no present is open. */
static const player_aux_rec_t* aux_curr(int a, int j) {
    return (s_pres_n && j >= 0 && j < s_store[s_pres_b].list.naux[a]) ? &s_store[s_pres_b].list.aux[a][j] : NULL;
}

/** @brief The previous list's auxiliary record bound to the model record that record j's model pairs with.
 *
 * @param a The auxiliary index.
 * @param j The record index.
 * @return The record, or NULL when `j` does not exist, its model record is
 *         unpaired, or the previous list has no record bound to the pair. */
static const player_aux_rec_t* aux_prev(int a, int j) {
    const player_aux_rec_t* d = aux_curr(a, j);
    const player_model_rec_t* m = d ? player_records_curr(d->model) : NULL;
    const store_slot_t* p = pres_slot(1);
    if (!m || m->pair < 0 || !p) return NULL;
    const rec_list_t* prv = &p->list;
    for (int i = 0; i < prv->naux[a]; i++)
        if (prv->aux[a][i].model == m->pair) return &prv->aux[a][i];
    return NULL;
}

/** @brief Prints one auxiliary record's binding and lerp state to stderr under OPENPETE_PLAYER_RECDBG.
 *
 * @param a The auxiliary index.
 * @param j The record index.
 * @param name The feeder's label in the line.
 * @param pos_name The staged position's label in the line.
 * @param staged The staged position's three words, or NULL to print zeros. */
static void aux_note(int a, int j, const char* name, const char* pos_name, const int32_t* staged) {
    if (!player_records_dbg()) return;
    const player_aux_rec_t* d = aux_curr(a, j);
    if (!d) return;
    const player_aux_rec_t* p = aux_prev(a, j);
    fprintf(stderr, "[recdbg %s f=%u j=%d] model=%d drawn=%d lerp=%d %s=(%d,%d,%d)\n",
            name, openpete_feed_key_now(), j, d->model, d->drawn, player_records_lerps(d->model) && p != NULL,
            pos_name, staged ? staged[0] : 0, staged ? staged[1] : 0, staged ? staged[2] : 0);
}

int player_records_shadow_count(void) { return aux_count(AUX_OF(PLAYER_REC_SHADOW)); }
const player_shadow_rec_t* player_records_shadow_curr(int j) { return aux_curr(AUX_OF(PLAYER_REC_SHADOW), j); }
const player_shadow_rec_t* player_records_shadow_prev(int j) { return aux_prev(AUX_OF(PLAYER_REC_SHADOW), j); }
void player_records_shadow_note(int j, const int32_t* staged_anchor) {
    aux_note(AUX_OF(PLAYER_REC_SHADOW), j, "shadow", "anchor", staged_anchor);
}
int player_records_flame_count(void) { return aux_count(AUX_OF(PLAYER_REC_FLAME)); }
const player_flame_rec_t* player_records_flame_curr(int j) { return aux_curr(AUX_OF(PLAYER_REC_FLAME), j); }
const player_flame_rec_t* player_records_flame_prev(int j) { return aux_prev(AUX_OF(PLAYER_REC_FLAME), j); }
void player_records_flame_note(int j, const int32_t* staged_origin) {
    aux_note(AUX_OF(PLAYER_REC_FLAME), j, "flame", "origin", staged_origin);
}
void player_records_present_note(int k, const int32_t* staged_pos) {
    if (!player_records_dbg()) return;
    const player_model_rec_t* c = player_records_curr(k);
    if (!c) return;
    fprintf(stderr, "[recdbg present f=%u k=%d] drawn=%d pair=%d pair2=%d lerp=%d alpha=%.3f pos=(%d,%d,%d)\n",
            openpete_feed_key_now(), k, c->drawn, c->pair, c->pair2, player_records_lerps(k), s_pres_alpha,
            staged_pos ? staged_pos[0] : 0, staged_pos ? staged_pos[1] : 0, staged_pos ? staged_pos[2] : 0);
}
void player_records_present_end(void) { s_pres_n = 0; s_pres_lerp = 0; s_pres_b = 0; }

op_time_t player_records_curr_ns(void) { return s_store[0].valid ? s_store[0].ns : 0; }

/* ---- module -------------------------------------------------------------- */

static feature_dispatch_entry_t s_dispatch[2];

/** @brief Installs the avatar draw hook in the dispatch table, or clears it.
 *
 * Entry 0 maps PL.hook_draw to on_player_draw when the layout names a hook and
 * the provider has run_draw; otherwise both entries stay empty (address 0, no
 * function). Entry 1 is always the empty terminator.
 * @param o Unused. */
static void player_apply_options(const openpete_options_t* o) {
    OP_ROLE_ASSERT(sim);
    (void)o;
    s_dispatch[0].addr = 0u; s_dispatch[0].fn = NULL;
    s_dispatch[1].addr = 0u; s_dispatch[1].fn = NULL;
    if (!PL.hook_draw || !player_game.run_draw) return;
    s_dispatch[0].addr = PL.hook_draw;
    s_dispatch[0].fn   = on_player_draw;
}

const feature_module_t player_module = {
    .name           = "player_native",
    .apply_options  = player_apply_options,
    .dispatch_table = s_dispatch,
};
