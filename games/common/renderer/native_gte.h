// games/common/renderer/native_gte.h - the ONE copy of the GTE
// float-mirror math shared by every native feeder of every game.
//
// Two projection families exist on purpose:
//
//   * ngte_project_raw - unclamped float H/z (moby, cyclorama). More
//     correct than the GTE for widescreen reveal; callers own their
//     near-plane policy on the returned view-Z.
//   * ngte_project_gte / ngte_screen_gte - the GTE-faithful pipeline
//     (IR16 saturation, SZ3 clamp, 0x1FFFF divide cap, optional ±1024
//     SXY bound) used where PSX screen positions must be matched to the
//     unit (spyro, flame, shadow, particle - particle drops the SXY
//     bound: widescreen reveal needs the true X).
//
// Near-plane z-floors deliberately stay in the CALLERS: they encode
// per-feeder policy (moby !=0->1.0, portal >1.0, terrain 1e-9, tracer
// reject ≤1.0) and unifying them is a behaviour change, not consolidation.

#ifndef OPENPETE_NATIVE_GTE_H
#define OPENPETE_NATIVE_GTE_H

#include <stdint.h>
#include "psx_runtime_internal.h"
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/* GTE UNR divide cap (0x1FFFF in 16.16) and SXY output saturation. */
#define NGTE_DIV_CAP   (131071.0 / 65536.0)
#define NGTE_SXY_BOUND 1024.0

/* The game's camera matrix scales its screen-Y row by this (row norms
 * 4096/2560/4096): the vertical FOV of a clip_h-row band is
 * 2·atan((clip_h/2) / (NGTE_Y_ROW · H)). */
#define NGTE_Y_ROW 0.625

/* One projection view per producer pass: every input a feeder needs to
 * project into PSX screen space, read once from the CPU state the pass
 * runs under (the staged g_Camera, which a held producer camera swaps) and
 * from the renderer's display policy. Built inside the producer-camera
 * bracket where one exists, so the bracket's integer-camera rule
 * (openpete_native_interp_pos returns 0 for the camera) reaches cam[].
 *
 * H_canon is the GTE's H: every hardware-limit mirror (IR16 saturation,
 * divide cap, near-fixup regime, UNR overflow regime) reads it. H_eff =
 * H_canon · s is the projection distance the feeder draws with; s is the
 * --fov scale (1.0 exactly at the default, so H_eff == H_canon
 * bit-for-bit). F / xlo / xhi are the Hor+ factor and its screen-X range;
 * extended is 1 when the view shows more than the canonical 4:3 frame
 * in either axis (F / s > 1 or s < 1) and the feeders must admit what the game's verdicts
 * dropped. */
typedef struct {
    int    cam_M[3][3];   /* g_Camera projection matrix (+0x00), Q12 */
    double cam_ram[3];    /* camera position, integer staged RAM (+0x28..0x30) */
    double cam[3];        /* camera position, sub-tick interpolated when
                           * available, else cam_ram */
    int    cam_interp;    /* 1 = cam[] came from the interp spine */
    double ofx, ofy;      /* gte_ctrl[24], [25] / 65536 */
    double H_canon;       /* gte_ctrl[26] */
    double s;             /* --fov scale, latched per present */
    double H_eff;         /* H_canon · s */
    float  F, xlo, xhi;   /* Hor+ factor and screen-X bounds */
    int    extended;      /* view wider than the canonical 4:3 frame */
} ngte_view;

/** @brief Builds the projection view from the CPU state and the display policy.
 *
 * Reads the camera matrix at game_layout.cam.base, the staged camera position
 * (the sub-tick interpolated one when the interp spine supplies it, else the
 * RAM value), the GTE screen offsets and H (control registers 24-26), the
 * --fov scale and the Hor+ factor with its screen-X bounds. The first call
 * latches OPENPETE_FOV_CENSUS. Writes no guest memory.
 * @param cpu The guest CPU state, read only.
 * @param v Receives the view. */
void ngte_view_build(CPUState* cpu, ngte_view* v);

/** @brief Whether the view shows more than the canonical 4:3 frame in either axis: F / s above 1 or s below 1.
 *
 * The one predicate behind ngte_view.extended and every feeder's widened-view
 * reveal. Reads the display policy and the GTE's H (control register 26);
 * writes no guest memory and no renderer state.
 * @param cpu The guest CPU state, read only.
 * @return 1 when the view is wider than 4:3 in either axis, else 0. */
int ngte_view_extended(CPUState* cpu);

struct openpete_occl_camera;
/** @brief Converts a view's camera into the form the occlusion grid compares.
 *
 * The position is in full-world units and the camera matrix has each row
 * scaled to unit length (a row shorter than 1e-6 is left unscaled). The game's
 * matrix is Q12 and not exactly orthonormal, so the rows are normalized here;
 * the grid's bound is then a plain nine-element dot product against the grid
 * camera's matrix.
 * @param v The view; supplies `cam` and `cam_M`.
 * @param c Receives the position, the row-major matrix and valid = 1. */
void ngte_occl_camera(const ngte_view* v, struct openpete_occl_camera* c);

/* Engagement census of the --fov policy (OPENPETE_FOV_CENSUS=1 arms it and
 * prints at exit). Every GTE hardware-limit mirror keys on H_canon; each
 * site also evaluates the same test at H_eff and both verdicts are counted,
 * so one run shows per site that the regime engaged (hit > 0), that the
 * display H would have moved it (eff != hit when s != 1) and that it did
 * not (hit is the same code at every FOV; at s = 1 the two are equal).
 * sxy_bound counts the SXY pin on the scaled output, which moves with s by
 * design. bb_untagged counts shaded-walk billboard records with no HUD tag
 * (expected 0: every billboard anchor is a game-authored screen
 * coordinate). reveal_admitted counts mobys the reveal admitted host-side
 * (0 at the canonical view), and reveal_spec_solo the subset of those whose
 * renderer byte selects the specular pass, which is the subset the walk gives
 * the special per-face material to. */
enum { NGTE_SITE_RAW_SAT, NGTE_SITE_GTE_SAT, NGTE_SITE_PART_CAP,
       NGTE_SITE_MOBY_NEAR, NGTE_SITE_TERRAIN_UNR, NGTE_SITE_N };
typedef struct {
    unsigned long hit[NGTE_SITE_N];   /* the limit test at H_canon */
    unsigned long eff[NGTE_SITE_N];   /* the same test at H_eff */
    unsigned long sxy_bound;
    unsigned long bb_untagged;
    unsigned long reveal_admitted;
    unsigned long reveal_spec_solo;
} ngte_census_t;
extern ngte_census_t ngte_census;
extern int ngte_census_on;
#define NGTE_CENSUS_HIT(field) \
    ((void)(ngte_census_on ? __atomic_fetch_add(&ngte_census.field, 1ul, __ATOMIC_RELAXED) : 0ul))
/* One limit site: canon is the verdict the code acts on, eff the verdict the
 * display H would have given. */
#define NGTE_LIMIT(site, canon, eff_) \
    ((void)(ngte_census_on ? ((canon) ? __atomic_fetch_add(&ngte_census.hit[site], 1ul, __ATOMIC_RELAXED) : 0ul, \
                              (eff_)  ? __atomic_fetch_add(&ngte_census.eff[site], 1ul, __ATOMIC_RELAXED) : 0ul) : 0ul))
/** @brief Copies `v` into `c` with s = 1.0 and H_eff = H_canon.
 *
 * The canonical view serves records whose screen position the game authored
 * (HUD, billboards) and instruments that compare against canonical prims.
 * @param v The view to copy.
 * @param c Receives the copy at the canonical FOV. */
void ngte_view_canon(const ngte_view* v, ngte_view* c);

/** @brief Unpacks a GTE SHORTMATRIX (5 words = 9 packed int16) from raw words.
 *
 * For callers whose source is the GTE control registers, not RAM. Each word
 * holds two sign-extended int16 elements, low half first, in row-major order;
 * the high half of w4 is unused.
 * @param w0 Elements [0][0] and [0][1].
 * @param w1 Elements [0][2] and [1][0].
 * @param w2 Elements [1][1] and [1][2].
 * @param w3 Elements [2][0] and [2][1].
 * @param w4 Element [2][2] in its low half.
 * @param M Receives the matrix, Q12. */
void ngte_unpack_matrix(uint32_t w0, uint32_t w1, uint32_t w2,
                        uint32_t w3, uint32_t w4, int M[3][3]);

/** @brief Unpacks a GTE SHORTMATRIX (5 words = 9 packed int16) from PSX RAM.
 *
 * Reads the five words at p..p+16 and decodes them with ngte_unpack_matrix.
 * @param cpu Reads guest RAM.
 * @param p Guest address of the matrix.
 * @param M Receives the matrix, Q12. */
void ngte_read_matrix(CPUState* cpu, uint32_t p, int M[3][3]);

/** @brief MAC1/2/3 = (M · v) / 4096, the rotate step alone with no TR and no divide.
 *
 * Computed in double with no truncation and no saturation.
 * @param M Rotation, Q12.
 * @param vx The vector's X.
 * @param vy The vector's Y.
 * @param vz The vector's Z.
 * @param m1 Receives MAC1.
 * @param m2 Receives MAC2.
 * @param m3 Receives MAC3. */
void ngte_mac(const int M[3][3], double vx, double vy, double vz,
              double* m1, double* m2, double* m3);

/** @brief The rotation half of an MVMVA through a unit-scale float matrix, in double: m = M · v.
 *
 * @param M Rotation, 1.0 = 1.0.
 * @param vx The vector's X.
 * @param vy The vector's Y.
 * @param vz The vector's Z.
 * @param m1 Receives row 0's product.
 * @param m2 Receives row 1's product.
 * @param m3 Receives row 2's product. */
void ngte_mac_f(const float M[3][3], double vx, double vy, double vz,
                double* m1, double* m2, double* m3);

/** @brief A Q12 matrix at unit scale in float; exact, since every Q12 entry fits a float mantissa.
 *
 * @param M The matrix, 4096 = 1.0.
 * @param out Receives M / 4096. */
void ngte_mat_unit_f(const int M[3][3], float out[3][3]);

/** @brief IR1/2/3 as the hardware hands them back with lm = 0: MAC saturated to signed 16 bits.
 *
 * A walk that mirrors a draw reading IR after MVMVA needs the saturation,
 * since the unsaturated MAC differs wherever the sector is far enough for the
 * product to overflow.
 * @param mac The MAC value.
 * @return mac clamped to -32768..32767. */
static inline double ngte_ir_clamp(double mac) {
    return mac < -32768.0 ? -32768.0 : (mac > 32767.0 ? 32767.0 : mac);
}

/** @brief ngte_project_raw through a unit-scale float rotation: MAC = TR + M · v.
 *
 * The int form converts its Q12 matrix with ngte_mat_unit_f and calls this
 * one; the conversion and the product are exact, so both forms return the
 * same value for the same Q12 matrix.
 * @param M Rotation, 1.0 = 1.0.
 * @param TR Translation, or NULL for zero.
 * @param vx The vertex's X.
 * @param vy The vertex's Y.
 * @param vz The vertex's Z.
 * @param ofx_px Screen X offset in pixels.
 * @param ofy_px Screen Y offset in pixels.
 * @param v The view; supplies H_eff and H_canon.
 * @param out_sx Receives the screen X.
 * @param out_sy Receives the screen Y.
 * @param out_sat Receives the saturation verdict, or NULL.
 * @return mac3, the view-space Z. */
double ngte_project_raw_f(const float M[3][3], const double TR[3],
                          double vx, double vy, double vz,
                          double ofx_px, double ofy_px, const ngte_view* v,
                          double* out_sx, double* out_sy, int* out_sat);

/** @brief Projects a vertex in unclamped float: screen = offset + MAC · H_eff / z.
 *
 * MAC = TR + (M · v) / 4096 with no saturation. The divisor is mac3, or 1.0
 * when mac3 is exactly zero; callers cull and clip on the returned mac3, which
 * carries no floor. ofx/ofy are per call (the shaded walk's billboards project
 * about their own anchor); H comes from the view. out_sat mirrors the GTE's
 * would-have-saturated verdict at H_canon: MAC1 or MAC2 outside int16, mac3 <= 0,
 * or H_canon >= 2 · mac3.
 * @param M Rotation, Q12.
 * @param TR Translation, or NULL for zero.
 * @param vx The vertex's X.
 * @param vy The vertex's Y.
 * @param vz The vertex's Z.
 * @param ofx_px Screen X offset in pixels.
 * @param ofy_px Screen Y offset in pixels.
 * @param v The view; supplies H_eff and H_canon.
 * @param out_sx Receives the screen X.
 * @param out_sy Receives the screen Y.
 * @param out_sat Receives the saturation verdict, or NULL.
 * @return mac3, the view-space Z. */
double ngte_project_raw(const int M[3][3], const double TR[3],
                        double vx, double vy, double vz,
                        double ofx_px, double ofy_px, const ngte_view* v,
                        double* out_sx, double* out_sy, int* out_sat);

/* GTE-faithful projection family output. m1/m2/m3 are the UNsaturated
 * MACs (particle stores them); sx/sy are post-IR16-clamp screen coords. */
typedef struct {
    double sx, sy;
    double m1, m2, m3;
    int sat;              /* SZ3 <= 0 or H >= 2*SZ3 (divide capped) */
} ngte_proj;

/** @brief The GTE-faithful perspective quotient for a view-space Z: H_eff / SZ3, or the divide cap scaled by the view's s.
 *
 * SZ3 is `m3` saturated to 0..0xFFFF. When SZ3 <= 0 or H_canon >= 2 * SZ3 the
 * GTE caps the divide at 0x1FFFF; the cap is NGTE_DIV_CAP * s, so the quotient
 * scales with --fov on both sides of the regime boundary. ngte_screen_gte
 * multiplies IR1 and IR2 by it.
 * @param v The view; supplies H_eff, H_canon and s.
 * @param m3 MAC3, the view-space Z.
 * @return The quotient, in pixels per view-space unit. */
double ngte_gte_quot(const ngte_view* v, double m3);

/** @brief The GTE-faithful divide half alone, for callers that already have view-space MACs.
 *
 * IR1/IR2 are the MACs saturated to int16 and SZ3 is MAC3 saturated to
 * 0..0xFFFF. When SZ3 <= 0 or H_canon >= 2 · SZ3 the quotient is the divide
 * cap NGTE_DIV_CAP scaled by the view's s, else H_eff / SZ3; the regime reads
 * H_canon so it does not move with --fov. With sxy_clamp the output is
 * bounded to -1024..1023 on each axis. o->m1..m3 keep the unsaturated MACs.
 * @param m1 MAC1.
 * @param m2 MAC2.
 * @param m3 MAC3.
 * @param ofx Screen X offset in pixels.
 * @param ofy Screen Y offset in pixels.
 * @param v The view; supplies H_eff, H_canon and s.
 * @param sxy_clamp Nonzero applies the SXY bound.
 * @param o Receives the screen position, the MACs and the saturation flag. */
void ngte_screen_gte(double m1, double m2, double m3,
                     double ofx, double ofy, const ngte_view* v,
                     int sxy_clamp, ngte_proj* o);

/** @brief Full GTE-faithful projection: MAC = TR + (M · v) / 4096, then ngte_screen_gte.
 *
 * @param M Rotation, Q12.
 * @param TR Translation, or NULL for zero.
 * @param vx The vertex's X.
 * @param vy The vertex's Y.
 * @param vz The vertex's Z.
 * @param ofx Screen X offset in pixels.
 * @param ofy Screen Y offset in pixels.
 * @param v The view.
 * @param sxy_clamp Nonzero applies the SXY bound.
 * @param o Receives the projection. */
void ngte_project_gte(const int M[3][3], const double TR[3],
                      double vx, double vy, double vz,
                      double ofx, double ofy, const ngte_view* v,
                      int sxy_clamp, ngte_proj* o);

/** @brief ngte_project_gte through a unit-scale float rotation: MAC = TR + M · v, then ngte_screen_gte.
 *
 * @param M Rotation, 1.0 = 1.0.
 * @param TR Translation, or NULL for zero.
 * @param vx The vertex's X.
 * @param vy The vertex's Y.
 * @param vz The vertex's Z.
 * @param ofx Screen X offset in pixels.
 * @param ofy Screen Y offset in pixels.
 * @param v The view.
 * @param sxy_clamp Nonzero applies the SXY bound.
 * @param o Receives the projection. */
void ngte_project_gte_f(const float M[3][3], const double TR[3],
                        double vx, double vy, double vz,
                        double ofx, double ofy, const ngte_view* v,
                        int sxy_clamp, ngte_proj* o);

/** One RTPS vertex as the GTE leaves it: MAC1-3, SZ3, the divide's quotient and SXY2. */
typedef struct {
    int32_t mac[3];       /**< MAC1-3 after the sf=1 shift, unsaturated */
    int32_t sz3;          /**< MAC3 saturated to 0..0xFFFF */
    int32_t q;            /**< the UNR quotient H / SZ3, capped at 0x1FFFF */
    int16_t sx, sy;       /**< SX2, SY2, saturated to -0x400..0x3FF */
} ngte_rtps_out;

/** @brief GTE RTPS (sf=1, lm=0), bit-exact integer mirror.
 *
 * MAC = (TR << 12) + M.V, >> 12; SZ3 saturates to 0..0xFFFF, IR1/2 to int16;
 * the divide is the UNR reciprocal of SZ3 capped at 0x1FFFF; SX/SY =
 * (IR * q + OF) >> 16, saturated to -0x400..0x3FF.
 * @param M   rotation, Q12
 * @param tr  translation (TRX, TRY, TRZ)
 * @param vx  VX0 as loaded (int16)
 * @param vy  VY0 as loaded (int16)
 * @param vz  VZ0 as loaded (int16)
 * @param ofx OFX in pixels
 * @param ofy OFY in pixels
 * @param H   the GTE's H
 * @param out MAC1-3, SZ3, the quotient and SXY2 */
void ngte_rtps_int(const int M[3][3], const int tr[3], int vx, int vy, int vz,
                   double ofx, double ofy, int H, ngte_rtps_out* out);

/** @brief One channel of GTE DPCS, the bit-exact integer mirror.
 *
 * MAC = c8 << 16; the far-colour difference ((fc12 << 12) - MAC) >> 12 is
 * saturated to int16, scaled by IR0 and added back, and the result shifted
 * right 16 in all and clamped to 0..255.
 * @param c8 The 8-bit colour channel.
 * @param fc12 The far colour channel expanded to 12 bits (RFC/GFC/BFC scale).
 * @param ir0 The 1.3.12 blend factor.
 * @return The depth-cued 8-bit channel. */
int ngte_dpcs_ch(int c8, int fc12, int ir0);

/** @brief GTE DPCS in float: every 8-bit channel of `rgb` lerped toward the matching channel of `fc` by ir0/4096.
 *
 * Each channel is rounded and clamped to 0..255. Both colours are packed
 * 0x00BBGGRR. The sky and portal-preview draws run the real op (cmd 0x10,
 * sf=1) over a colour table with FC = the fog far colour's components SHL 4
 * and IR0 = the fog factor; FC SHL 12 then lines up with the colour's SHL 16,
 * so the op reduces to this per-channel lerp. The float form is what the rest
 * of the native path carries; ngte_dpcs_ch is the bit-exact integer mirror
 * for the walks that must match the GTE to the unit.
 * @param rgb The source colour.
 * @param fc The far colour.
 * @param ir0 The blend factor, 4096 = the far colour.
 * @return The blended colour, packed 0x00BBGGRR. */
uint32_t ngte_depth_cue(uint32_t rgb, uint32_t fc, int ir0);

/** @brief The fog far colour (game_layout.fog.far_colour) as packed 0x00BBGGRR.
 *
 * The source is one word holding the three 8-bit components, or, when
 * fog.far_colour_words is set, three words R, G, B at the GTE's RFC/GFC/BFC
 * scale (16 times the 8-bit value), each shifted right 4 and masked to 8 bits.
 * @param cpu Reads guest RAM.
 * @return The packed colour, or 0 when the game leaves the address unset. */
uint32_t ngte_fog_far_colour(CPUState* cpu);

/** @brief O = (A · B) >> 12 on Q12 3x3 matrices, the column-rotate the game does via MVMVA.
 *
 * Accumulates in 64 bits: the GTE's MAC is 44 bits wide, and a scaled model
 * matrix times a rotation exceeds int32 before the shift.
 * @param A The left factor, Q12.
 * @param B The right factor, Q12.
 * @param O Receives the product, Q12; must not alias A or B. */
void ngte_mat_mul3(const int A[3][3], const int B[3][3], int O[3][3]);

/* The game's in-RAM sin/cos table (game_layout.sin_table): 256 entries per
 * circle, int16 Q12, cos at +0x80 bytes. A byte angle indexes it at byte<<1.
 * A game whose table is unlocated (the field is 0) gets sin 0 / cos 4096 from
 * every helper below instead of a read through address 0. */

/** @brief sin/cos at fractional byte-angle resolution, lerping adjacent table entries.
 *
 * Exact at integer angles and linear between, so a rotation renders below the
 * 1.4 degree byte grid. fb wraps into [0,256); entry 255 lerps toward entry 0.
 * An unlocated table gives sin 0 and cos 4096.
 * @param cpu Reads the game's sin/cos table.
 * @param fb The byte angle, 256 to the turn.
 * @param s_out Receives sin, Q12 scale.
 * @param c_out Receives cos, Q12 scale. */
void ngte_sincos_f(CPUState* cpu, double fb, double* s_out, double* c_out);

/** @brief ngte_sincos_f rounded to the nearest int (halves away from zero) for Q12 matrix composition.
 *
 * @param cpu Reads the game's sin/cos table.
 * @param fb The byte angle, 256 to the turn.
 * @param s_out Receives sin, Q12.
 * @param c_out Receives cos, Q12. */
void ngte_sincos_interp(CPUState* cpu, double fb, int* s_out, int* c_out);

/** @brief Projects a camera-relative offset the way every self-linking producer in this engine does.
 *
 * off is ordered (X - camX, camY - Y, camZ - Z) in the pool's own world units;
 * the camera matrix rotates the vector (off[1], off[2], off[0]) with a zero
 * translation and ngte_screen_gte divides it about the view's ofx/ofy. The SXY
 * bound is dropped, as the particle feeder drops it, because widescreen reveal
 * needs the true X.
 * @param v The view.
 * @param off The camera-relative offset.
 * @param o Receives the projection. */
void ngte_project_offset(const ngte_view* v, const double off[3], ngte_proj* o);

/** @brief The rotation carrying screen-space corner extents from the table vector one angle names to the vector another names.
 *
 * The result is the cos/sin of the change. Both angles are in table entries,
 * 256 to the turn, read through ngte_sincos_f; a routine that indexes the table
 * at a fixed offset from its record's angle byte folds that offset into both.
 * The extents ride the table's own vector, so the change is the rotation
 * between the two table vectors, divided by the squared amplitude of the first:
 * exact whatever the table's phase, sense and amplitude. An unlocated table
 * reads sin 0 / cos 4096 at every angle and so yields the identity with a
 * return of 1.
 * @param cpu Reads the game's sin/cos table.
 * @param from_ang The source angle.
 * @param to_ang The destination angle.
 * @param rc Receives the cos of the change.
 * @param rs Receives the sin of the change.
 * @return 1 when a rotation was computed; 0 with the identity written when the
 *         angles are equal or the first table vector's squared length is below 1. */
int ngte_table_rot(CPUState* cpu, double from_ang, double to_ang,
                   double* rc, double* rs);

typedef struct { int sy, cy, sx, cx, sz, cz; } ngte_euler_sc;
/** @brief Looks up the table sine and cosine of each axis of a packed euler word.
 *
 * The word packs Z in byte 0, X in byte 1 and Y in byte 2 (m_Rotation@+0x44).
 * Each axis reads its sine at game_layout.sin_table + 2 * angle and its cosine
 * 0x80 bytes on, as sign-extended halfwords. rot == 0 gives sin 0 / cos 4096 on
 * every axis, as does a game whose table is not located.
 * @param cpu Reads the game's sin/cos table.
 * @param rot The packed euler word.
 * @param e Receives the three axes' sine and cosine. */
void ngte_euler_sincos(CPUState* cpu, uint32_t rot, ngte_euler_sc* e);

/** @brief out = start·Ry·Rx·Rz in Q12, the order the game's MVMVA sequence applies.
 *
 * func_80022A2C block_80022DBC..80022F34. Each product goes through
 * ngte_mat_mul3, so every step truncates by >> 12 as the GTE does.
 * @param start The left factor, Q12.
 * @param e The three axes' table sin/cos (ngte_euler_sincos).
 * @param out Receives the product, Q12. */
void ngte_compose_euler(const int start[3][3], const ngte_euler_sc* e, int out[3][3]);

/** @brief out = start·Ry·Rx·Rz in double, the order of ngte_compose_euler, at fractional byte angles.
 *
 * Each axis's sin/cos is read through ngte_sincos_f at a fractional byte angle.
 * ang is {Z, X, Y} in byte angles, 256 to the turn (the packed euler word's
 * byte0, byte1, byte2). At integer angles the factors are the table's own
 * entries, so the result differs from the GTE compose only by the GTE's
 * per-step >>12 truncation, which this form does not take.
 * @param cpu    reads the game's sin/cos table.
 * @param start  the left factor, Q12.
 * @param ang    {Z, X, Y} byte angles.
 * @param out    the product, Q12 scale, unrounded. */
void ngte_compose_euler_f(CPUState* cpu, const int start[3][3], const double ang[3],
                          double out[3][3]);

/** @brief The packed euler word's three angles as byte angles {Z, X, Y}, the order ngte_compose_euler_f and ngte_compose_euler_mf take.
 *
 * @param rot The packed euler word (Z at byte 0, X at byte 1, Y at byte 2).
 * @param ang Receives {Z, X, Y}, 256 to the turn. */
void ngte_word_angles(uint32_t rot, double ang[3]);

/** @brief O = A · B for unit-scale float matrices, accumulated in float.
 *
 * @param A The left factor.
 * @param B The right factor.
 * @param O Receives the product; must not alias A or B. */
void ngte_mat_mul3_f(const float A[3][3], const float B[3][3], float O[3][3]);

/** @brief out = start·Ry·Rx·Rz at unit scale in float, from the game's sine table at fractional byte angles.
 *
 * The axis order and signs are ngte_compose_euler's. Each axis reads
 * ngte_sincos_f (linear between table entries, the entry itself at an
 * integer angle) scaled by 1/4096; the three products accumulate in float
 * (ngte_mat_mul3_f) with no per-step truncation.
 * @param cpu Reads the game's sin/cos table.
 * @param start The left factor at unit scale.
 * @param ang {Z, X, Y} byte angles.
 * @param out Receives the product at unit scale. */
void ngte_compose_euler_mf(CPUState* cpu, const float start[3][3], const double ang[3],
                           float out[3][3]);

/** @brief Compose start·Ry·Rx·Rz at fractional byte angles in both forms: Q12 as the GTE does it, and at unit scale in float.
 *
 * The Q12 form samples the table at the angles rounded to integers
 * (ngte_sincos_interp) and truncates each product (ngte_compose_euler); the
 * float form converts start (ngte_mat_unit_f) and composes unrounded
 * (ngte_compose_euler_mf).
 * @param cpu Reads the game's sin/cos table.
 * @param start The left factor, Q12.
 * @param ang {Z, X, Y} byte angles.
 * @param out Receives the product, Q12.
 * @param outf Receives the product at unit scale, or NULL. */
void ngte_compose_euler_at(CPUState* cpu, const int start[3][3], const double ang[3], int out[3][3],
                           float outf[3][3]);

/* ---- texture page / CLUT bitfields --------------------------------------
 * One decode of the GPU's two texture words, shared by every feeder that
 * samples VRAM. `tp` is the texture-page bitfield (bits 0-3 page X in
 * 64-texel columns, bit 4 page Y in 256-line halves, bits 5-6 the
 * semi-transparency rate ABR, bits 7-8 the colour depth); `clut` is the
 * CLUT bitfield (bits 0-5 X in 16-entry columns, bits 6-14 Y in lines).
 * Callers own their ABE/STP predicate: this reports the raw ABR and leaves
 * the blend decision to ngte_abr_blend. */
typedef struct {
    int tpx, tpy;      /* texture page origin, VRAM texels / lines */
    int clx, cly;      /* CLUT origin, VRAM texels / lines */
    int bpp;           /* 4, 8 or 16; 32 for the replaced-texture escape */
    int abr;           /* raw semi-transparency rate, 0..3 */
    int mode;          /* raw colour-depth field, 0..3 */
    int replaced_id;   /* replaced-texture id, or -1 when there is none */
} ngte_tpage;

/* Colour depth 3 is reserved by the hardware, so an emitter can spend it on
 * naming a replaced texture whose id rides in the CLUT half. With this flag
 * mode 3 yields bpp 32, tpx = the signed id, replaced_id = the same id and a
 * zeroed CLUT. Without it mode 3 decodes as an ordinary 16bpp page, which is
 * what a feeder that never sees the escape expects. */
#define NGTE_TP_REPLACED_ID 0x1u

/** @brief Decodes a texture-page word and a CLUT word into origins, colour depth and blend rate.
 *
 * `tp` bits 0-3 are the page X in 64-texel columns, bit 4 the page Y in
 * 256-line halves, bits 5-6 the semi-transparency rate ABR and bits 7-8 the
 * colour depth (0 is 4 bpp, 1 is 8 bpp, otherwise 16 bpp). `clut` bits 0-5 are
 * the CLUT X in 16-entry columns and bits 6-14 its Y in lines. The ABR is
 * reported raw; the caller owns its blend-enable predicate and turns the rate
 * into a blend code with ngte_abr_blend. With NGTE_TP_REPLACED_ID and depth 3
 * the result is the replaced-texture escape described at that flag.
 * @param tp The texture-page bitfield.
 * @param clut The CLUT bitfield.
 * @param flags NGTE_TP_* bits, 0 for none.
 * @param out Receives the decode; every field is written. */
void ngte_decode_tpage(unsigned tp, unsigned clut, unsigned flags, ngte_tpage* out);

/* ABR to the renderer's blend code: 1 = (B+F)/2, 2 = B+F, 3 = B-F,
 * 4 = B+F/4. NGTE_ABR_EXACT is the hardware's own table. A feeder with no
 * B+F/4 pipeline passes NGTE_ABR_QUARTER_AS_ADD, which draws rate 3
 * additive. Blend code 0 (opaque) is the caller's, from its own ABE bit. */
typedef enum {
    NGTE_ABR_EXACT = 0,
    NGTE_ABR_QUARTER_AS_ADD = 1
} ngte_abr_policy;

/** @brief Maps a raw semi-transparency rate to the renderer's blend code.
 *
 * Rates 0, 1 and 2 give codes 1 ((B+F)/2), 2 (B+F) and 3 (B-F); rate 3 gives
 * code 4 (B+F/4), or code 2 under NGTE_ABR_QUARTER_AS_ADD. Blend code 0
 * (opaque) is the caller's, from its own blend-enable bit.
 * @param abr The raw rate; only the low two bits are read.
 * @param policy How rate 3 maps.
 * @return The blend code, 1 to 4. */
int ngte_abr_blend(int abr, ngte_abr_policy policy);

/* ---- screen-space backface test ----------------------------------------
 * Signed 2*area of a screen triangle: the sign of the GTE's NCLIP MAC0,
 * which the game tests to drop a face. NGTE_NCLIP_SXY_GRID rounds each
 * coordinate to the GTE's int16 SXY grid first, so the verdict matches the
 * .s face for face; NGTE_NCLIP_FLOAT keeps float precision, which stops a
 * near-zero-area fan flipping sign at grazing angles and popping. */
typedef enum {
    NGTE_NCLIP_FLOAT = 0,
    NGTE_NCLIP_SXY_GRID = 1
} ngte_nclip_mode;

/** @brief The signed doubled area of a screen triangle, the sign of the GTE's NCLIP value.
 *
 * ax * (by - cy) + bx * (cy - ay) + cx * (ay - by), with each coordinate first
 * rounded to the integer SXY grid under NGTE_NCLIP_SXY_GRID.
 * @param ax The first vertex X.
 * @param ay The first vertex Y.
 * @param bx The second vertex X.
 * @param by The second vertex Y.
 * @param cx The third vertex X.
 * @param cy The third vertex Y.
 * @param mode Whether to round to the SXY grid first.
 * @return The signed doubled area. */
double ngte_nclip(double ax, double ay, double bx, double by,
                  double cx, double cy, ngte_nclip_mode mode);

/** @brief Hex dump of `nwords` guest words from `base`, eight per line, under `label`, to stderr.
 *
 * A verify-mode diagnostic.
 * @param cpu Reads guest RAM.
 * @param label Printed before the base address.
 * @param base Guest address of the first word.
 * @param nwords Words to print. */
void ngte_dump_words(CPUState* cpu, const char* label, uint32_t base, int nwords);

/* ---- ordering-table polybuf walk ----------------------------------------
 * A polybuf is a run of primitives laid out back to front. Each primitive
 * opens with its ordering-table tag: the tag's high byte is the primitive's
 * length in words, so the next primitive starts ((w0 >> 24) + 1) * 4 bytes
 * on, and the low 24 bits are the OT link. Word 1's high byte is the GPU
 * command code; codes 0x20..0x3F are polygons, with bit 4 gouraud, bit 3
 * quad, bit 2 textured and bit 1 semi-transparent, and the vertex words
 * follow in that order. A code outside that range (tiles, lines, draw-mode
 * mini-packets) decodes to nv = 0 and carries only its raw words.
 *
 * ngte_walk_prims is the one walk. Every bound a feeder disagrees on rides
 * in the policy; the decoded primitive goes to the feeder's sink, which
 * stores it in the feeder's own array, so no feeder's stored form changes.
 */
#define NGTE_PRIM_RAW_MAX 20

typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1] >> 24) */
    uint8_t  taglen;        /* OT tag length (word[0] >> 24) */
    int      nv;            /* 3 or 4 for a polygon, 0 otherwise */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad, stp;
    uint32_t raw[NGTE_PRIM_RAW_MAX];  /* the packet's words, tag included; words
                                       * past the packet read zero */
    int      nraw;          /* raw words the policy asks the sink to use,
                             * capped by raw_words_max */
} ngte_prim;

/* Stores one primitive. Returns nonzero when it was stored: a sink whose
 * array is full returns 0, and the walk then leaves it out of the counts it
 * keeps per stored primitive. */
typedef int (*ngte_prim_sink)(void* ctx, const ngte_prim* prim);

typedef struct {
    uint32_t  tag_len_max;   /* a longer tag ends the walk as corrupt */
    int       raw_words_max; /* raw words the sink receives, 0 for none */
    int       guard;         /* primitives walked before the walk gives up */
    int       polygons_only; /* 1 = the sink sees polygons only */
    uint32_t* code_hist;     /* [256] by code byte, or NULL */
    uint32_t* tag_hist;      /* [256] by tag length, or NULL */
    uint32_t* total;         /* primitive counter, or NULL */
    int       count_all;     /* 1 = count every primitive walked, 0 = only
                              * the ones the sink stored */
} ngte_walk_policy;

/** @brief Decodes one primitive packet from its words, as the polybuf walk decodes each packet it stores.
 *
 * The code is word 1's high byte and the tag length word 0's. A polygon (code
 * 0x20-0x3F) is decoded into vertex count, positions, 24-bit colours, UVs,
 * CLUT, tpage and flags; any other code carries only its code, tag length and
 * raw words. Words past `words` read zero.
 * @param w The packet's words, tag first; at least two.
 * @param words How many of `w` the packet holds, capped at NGTE_PRIM_RAW_MAX.
 * @param raw_max The raw words the consumer reads (stored in nraw, capped by `words`).
 * @param t Receives the decoded primitive. */
void ngte_prim_decode(const uint32_t* w, int words, int raw_max, ngte_prim* t);

/** @brief Walks the polybuf interval [start, end) primitive by primitive, decoding each for a sink.
 *
 * Each step advances (tag length + 1) * 4 bytes. The walk ends at `end`, after
 * pol->guard primitives, or at a tag longer than pol->tag_len_max. A primitive
 * whose tag length is zero is stepped by the length its polygon code implies,
 * and a zero-length tag on a non-polygon code ends the walk. A polygon (code
 * 0x20-0x3F) is decoded into vertex count, positions, 24-bit colours, UVs,
 * CLUT, tpage and flags; any other code carries only its code, tag length and
 * raw words, and is withheld from the sink when pol->polygons_only. The
 * histograms and total count every walked primitive under pol->count_all,
 * else only the ones the sink stored. Reads guest memory, writes none.
 * @param cpu The guest CPU state, read only.
 * @param start Guest address of the first primitive.
 * @param end Guest address one past the interval.
 * @param pol The walk's bounds and counters.
 * @param sink Called once per primitive; a nonzero return means it stored it.
 * @param ctx Passed to the sink unchanged. */
void ngte_walk_prims(CPUState* cpu, uint32_t start, uint32_t end,
                     const ngte_walk_policy* pol, ngte_prim_sink sink, void* ctx);

/* ---- flat-depth screen primitive feed ------------------------------------
 * A feeder that replays a captured screen-space primitive at one depth (a
 * particle, a gem glint, a zap or tracer ribbon segment) hands its corners
 * here; the corners may already carry the feeder's rebase or widening. */

/** @brief The texture page and native blend of a captured polygon, from its own tpage and CLUT words.
 *
 * A textured primitive decodes its page (no flags) and, when
 * semi-transparent, blends by its ABR with rate 3 drawn additive
 * (NGTE_ABR_QUARTER_AS_ADD); an untextured one zeroes `tp` and blends with
 * code 1 when semi-transparent. Opaque is 0.
 * @param p The captured primitive.
 * @param tp Receives the decoded page; zeroed for an untextured primitive.
 * @return The native blend code. */
int ngte_prim_tpage_blend(const ngte_prim* p, ngte_tpage* tp);

/** @brief Feeds a three- or four-corner screen-space primitive at one depth to the native scene's world layer.
 *
 * A quad (`nv` 4) feeds corners (0,1,2) and (1,3,2), the GPU's split; a
 * triangle (any other `nv`) feeds (0,1,2) alone and corner 3 is not read. Every
 * vertex takes `depth` as its Z and as its W, so the attributes interpolate
 * screen-linearly, and each colour's low 24 bits with full alpha.
 * @param x The corners' screen X, PSX pixels.
 * @param y The corners' screen Y, PSX pixels.
 * @param u The corners' texture U.
 * @param v The corners' texture V.
 * @param rgb The corners' colour words.
 * @param depth The scene depth.
 * @param tp The texture page and CLUT to sample, or NULL for untextured.
 * @param blend The native blend code, 0 for opaque.
 * @param nv The primitive's corner count, 4 for a quad, 3 for a triangle.
 * @return The number of triangles fed. */
int ngte_feed_flat_quad(const float x[4], const float y[4], const float u[4],
                        const float v[4], const uint32_t rgb[4], float depth,
                        const ngte_tpage* tp, int blend, int nv) OP_REQUIRES(sim);

/** @brief Feeds a screen-space primitive as ngte_feed_flat_quad does, each corner's colour carrying its residual and the primitive its bucket.
 *
 * A bucket of 0 or more stamps every triangle through
 * openpete_scene_tri_set_bucket, with the aura contest when the primitive is
 * additive (blend 2): the routine appends it at its bucket's tail, after the
 * moby faces spliced there, so it loses a pixel to an actor or player key in
 * a nearer bucket and keeps true depth against terrain. -1 feeds exactly as
 * ngte_feed_flat_quad.
 * @param x The corners' screen X, PSX pixels.
 * @param y The corners' screen Y, PSX pixels.
 * @param u The corners' texture U.
 * @param v The corners' texture V.
 * @param rgb The corners' colour words.
 * @param rgbq The corners' colour residuals (ngte_cvert.rgbq), or NULL for whole bytes.
 * @param depth The scene depth.
 * @param tp The texture page and CLUT to sample, or NULL for untextured.
 * @param blend The native blend code, 0 for opaque.
 * @param nv The primitive's corner count, 4 for a quad, 3 for a triangle.
 * @param bucket The ordering-table bucket the routine links the primitive at, or -1 when unknown.
 * @return The number of triangles fed. */
int ngte_feed_flat_quad_q(const float x[4], const float y[4], const float u[4],
                          const float v[4], const uint32_t rgb[4], const uint32_t rgbq[4],
                          float depth, const ngte_tpage* tp, int blend, int nv, int bucket) OP_REQUIRES(sim);

/** @brief Builds the quad that covers the pixels the PSX GPU draws for a line.
 *
 * The GPU steps k = max(|dx|, |dy|) times along the major axis and draws
 * k + 1 pixels, both endpoints included, each at the minor coordinate rounded
 * from the exact line. The quad is the parallelogram through the two endpoint
 * pixel centres, extended half a pixel past each end along the major axis
 * and half a pixel to each side along the minor axis; at 1x a pixel centre
 * falls inside it exactly when the GPU draws that pixel, except for centres
 * on its edge, where the minor coordinate rounds a tie. A zero-length line is
 * the endpoint's pixel; |dx| == |dy| is X-major. Corners 0 and 1 are the
 * start and end on the minus-minor side, 2 and 3 on the plus side, the order
 * ngte_feed_flat_quad splits as (0,1,2) and (1,3,2).
 * @param ax Endpoint a's screen X, PSX pixels.
 * @param ay Endpoint a's screen Y.
 * @param bx Endpoint b's screen X.
 * @param by Endpoint b's screen Y.
 * @param centre Added to each coordinate to reach the endpoint pixel centre:
 *        0.5 for the guest's integer pixel coordinates, 0 for a GTE float
 *        projection, whose truncation to the guest's integer leaves the
 *        float inside the endpoint's pixel.
 * @param qx Receives the four corners' X.
 * @param qy Receives the four corners' Y. */
void ngte_line_quad(float ax, float ay, float bx, float by, float centre,
                    float qx[4], float qy[4]);

/** @brief Feeds a screen-space line between two GTE float endpoints at one depth as the untextured quad ngte_line_quad builds.
 *
 * The quad covers the pixels the GPU draws for the line at the centre
 * offset ngte_line_quad takes. Corners 0 and 2 take endpoint a's colour, 1 and 3 endpoint b's, fed through
 * ngte_feed_flat_quad as two triangles.
 * @param ax Endpoint a's screen X, PSX pixels.
 * @param ay Endpoint a's screen Y.
 * @param bx Endpoint b's screen X.
 * @param by Endpoint b's screen Y.
 * @param rgb_a Endpoint a's colour word.
 * @param rgb_b Endpoint b's colour word.
 * @param centre 0 for float endpoints, 0.5 for the guest's integer pixel corners.
 * @param depth The scene depth.
 * @param blend The native blend code, 0 for opaque.
 * @return The number of triangles fed. */
int ngte_feed_flat_line(float ax, float ay, float bx, float by, uint32_t rgb_a,
                        uint32_t rgb_b, float centre, float depth, int blend) OP_REQUIRES(sim);

/* ---- near-plane clip corner --------------------------------------------- */
/*
 * A corner in camera space, the form a Sutherland-Hodgman cut against a
 * constant-view-Z plane interpolates: position (m1, m2, m3), colour, UV.
 *
 * osx/osy/oflag carry an optional SCREEN-space override that a producer's
 * push step uses instead of projecting m1/m2/m3. A producer that projects
 * every corner leaves oflag zero. The terrain's near/HQ subdivide sets it on
 * its boundary midpoints under the integer vertex snap: a midpoint projected
 * from camera space lands sub-pixel off the parent's projected edge
 * (perspective), and the snap amplifies that into a visible T-junction crack
 * against the unsubdivided neighbour sharing that edge. The override places
 * the midpoint at the exact screen midpoint of the snapped parent corners
 * (integer or exact-.5, which the renderer's snap preserves), collinear with
 * the neighbour's snapped edge.
 *
 * fx/fy: a terrain producer's face-local coordinate of the corner (the
 * near drain's grid in quarter-edge units, psx_key_contest.glsl), carried
 * through a cut as the UVs are. Zero where no producer sets it.
 *
 * rgbf: a terrain producer's far (depth-cue target) colour beside the near
 * colour in rgb, for a node it fogs before the cut. A cut does not carry it.
 *
 * rgbq: the residual of rgb's three bytes from a node's unrounded colour,
 * three signed bytes in 1/256 of a level (rhi_vertex.rgbq). Zero on a
 * vertex whose colour is whole bytes. A cut carries it (ngte_rgb_mix).
 */
typedef struct { double m1, m2, m3; uint32_t rgb; float u, v;
                 float osx, osy; uint8_t oflag;
                 float fx, fy;
                 uint32_t rgbf; uint32_t rgbq; } ngte_cvert;

/** @brief Mixes two 0x00BBGGRR colours, each with its residual, per channel at a weight, rounded to the nearest byte.
 *
 * Each channel's level is its byte plus its residual's signed byte / 256;
 * the mix of the two levels rounds half up to a byte clamped to [0, 255],
 * and the mix less that byte, in 1/256 of a level rounded half up and
 * clamped to [-128, 127], is the channel's residual. The mix less its byte
 * lies in [-0.5, 0.5), so the byte and residual together reproduce the
 * unrounded mix to 1/512 of a level, and to under 1/256 where the mix lies
 * within 1/512 below a half level and the residual's 128 clamps to 127; a
 * node or a cut placed anywhere on the edge keeps the colour the edge
 * interpolates to that bound.
 * @param a The colour at weight 0.
 * @param aq Its residual (ngte_cvert.rgbq), 0 for whole bytes.
 * @param b The colour at weight 1.
 * @param bq Its residual.
 * @param t The weight of b.
 * @param q Receives the mix's residual; NULL drops it.
 * @return The mixed colour's bytes. */
uint32_t ngte_rgb_mix(uint32_t a, uint32_t aq, uint32_t b, uint32_t bq, double t, uint32_t* q);

/** @brief The point where the edge a->b crosses the plane m3 == zc.
 *
 * Position, UV and the face-local coordinate interpolate linearly in camera
 * space; the colour is ngte_rgb_mix of the corners' colours, its residual
 * kept in rgbq, and the far colour rgbf is not carried. The parameter is clamped to
 * [0,1], so an edge that does not cross yields the attributes of whichever
 * endpoint the caller's inside test already picked, with m3 set to zc. The
 * result projects normally: a crossing point is not one of the parent
 * corners, so it carries no screen override.
 * @param a The edge's first corner.
 * @param b The edge's second corner; its m3 must differ from a's.
 * @param zc The plane's depth.
 * @return The crossing corner. */
ngte_cvert ngte_clip_lerp(ngte_cvert a, ngte_cvert b, double zc);

/* One output corner of a near-plane clip: input corner a when cross is 0,
 * else the crossing of edge a->b at parameter t (0 at a, 1 at b), the
 * parameter ngte_clip_lerp computes for the same edge and plane. */
typedef struct { uint8_t a, b, cross; double t; } ngte_clip_ref;

/** @brief Sutherland-Hodgman clip of a triangle against the plane z >= zc, as a list of output corners.
 *
 * Walking the edges a->b in order (0->1, 1->2, 2->0), each corner on or in
 * front of the plane is kept and each edge whose ends lie on opposite sides
 * adds its crossing: 0 corners behind give the triangle, 1 gives a quad, 2
 * give a triangle, 3 give nothing. The cut is made once, at zc; a caller
 * whose clip position's w is the view depth leaves a crossing that projects
 * far off screen to the rasterizer's clip. The caller fans the output from
 * corner 0.
 * @param z The three corners' depths.
 * @param zc The plane's depth.
 * @param out Receives the output corners.
 * @param clipped Receives 1 when a corner was cut, else 0.
 * @return The output corner count: 3 or 4, or 0 when the face lies behind the plane. */
int ngte_near_clip_tri(const double z[3], double zc, ngte_clip_ref out[4], int* clipped);

#endif /* OPENPETE_NATIVE_GTE_H */
