// games/common/renderer/flame.c - the player's fire-breath ribbon, re-derived
// from gamestate in float, and its numerical A/B harness. An ADDITIVE-blend
// consumer (ABR mode 1).
//
// The emitter is a self-contained handwritten GTE routine (Spyro 1
// func_80058D64 / r_flame.s, Spyro 2 func_80048F04, Spyro 3 func_8002D2C4;
// the same 450 instructions but for constants and symbols): a procedurally
// generated billboard ribbon strip with sine-table flicker, two-stage
// transform (flame origin -> camera space, then the flame-attach matrix the
// player renderer cached this tick), additively blended.
//
// The strip cell is 0x34 bytes = 13 words = POLY_GT4 and the colour/code word
// is 0x3E (poly+gouraud+QUAD+TEXTURED+semi-transparent+modulate), so the strip
// is a textured semi-transparent gouraud quad ribbon; the first cell's caps
// are untextured gouraud triangles (code 0x30). Every per-game address,
// stride, amplitude and bias comes from game_layout.flame; the walk below is
// one body.
//
// Determinism: the hook calls the real renderer exactly once (behaviour-
// identical to normal dispatch) and only READS PSX RAM afterwards. The
// present-time flame_records_present overwrites a staged copy of the flame
// span, or the span in PSX RAM itself when staging is refused, and restores it.
// Gated by OPENPETE_FLAME_VERIFY (dump/A/B) or a native renderer mode
// (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "psx_runtime_internal.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"
#include "openpete_log.h"  /* OPL_ERROR_F - ABR/blend FIXME runtime alarms */
#include "game_layout.h"
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "renderer/player.h" /* per-call records of the player hooks */
#include "renderer/flame.h"
#include "openpete_subtick_view.h"  /* the host-only staged view of the flame span */
#include "openpete_psx.h"           /* the PSX-to-host pointer fallback */
#include "renderer/interp_lerp.h"   /* lerp_vec3 / lerp_matrix for the bracket */
#include "renderer/env_knob.h"      /* env_frame_range_t - the census window */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE - fiber-hook cost split */
#include "renderer/player_hide.h"
#include "openpete_thread_roles.h"

extern unsigned openpete_frame;

/* Sub-keyframe slide (0..1): the fractional part of the per-strand counter the
 * spine is advancing this present. The counter selects the ribbon's window into
 * the STATIC cross-section spine (one 8-byte record per counter step); this frac
 * slides each cross-section between record[p] and record[p+8] so the ribbon
 * animates BELOW the keyframe grid (matching the avatar's keyframe-lerp
 * smoothness) instead of stepping one record at a time. Set by
 * flame_records_present before each per-record re-extract; 0 on the canonical
 * render (exact keyframes). */
static double s_flame_subframe = 0.0;
void flame_native_set_subframe(double frac) {
    s_flame_subframe = (frac < 0.0) ? 0.0 : (frac > 1.0 ? 1.0 : frac);
}

/* ---- the per-game descriptor ---------------------------------------------- */
#define FL       (game_layout.flame)
#define FL_STATE (game_layout.player.flame_base)

/* Offsets every game shares in the flame state block. */
#define FL_POS        0x00u         /* Vector3D m_Position (origin)            */
#define FL_ACTIVE     0x98u         /* m_IsFlameActive                         */
#define FL_CNT        0x20u         /* per-strand vertex count [i] (lb)        */
#define FL_LEN        0x28u         /* per-strand length [i] (lb)              */

/** @brief Whether the flame is in its wider second form.
 *
 * Reads the word at game_layout.flame.super_off in the flame state block.
 * @param cpu The guest CPU state, read only.
 * @return 1 when the word is nonzero, 0 otherwise or when the game's
 *         descriptor names no such word. */
static int flame_super(CPUState* cpu) {
    if (!FL.super_off) return 0;
    return (int)cpu->read_word(FL_STATE + FL.super_off) != 0;
}

/* ---- captured prims (ground truth) --------------------------------------- */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1]>>24) */
    uint8_t  len;           /* OT tag length (word[0]>>24)      */
    int      nv;            /* 3 or 4                           */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
    uint32_t raw[20];
    int      nraw;
} prim_t;

#define MAX_PRIMS 4096
static prim_t s_gte[MAX_PRIMS]; static int s_gte_n;
static uint32_t s_code_hist[256];

/** @brief Store one walked primitive into the capture buffer s_gte.
 *
 * @param ctx Unused.
 * @param s The decoded primitive.
 * @return 1 when stored, 0 when the buffer is full. */
static int flame_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_gte_n >= MAX_PRIMS) return 0;
    prim_t* t = &s_gte[s_gte_n];
    memset(t, 0, sizeof(*t));
    t->code = s->code; t->len = s->taglen; t->nv = s->nv;
    for (int k = 0; k < 4; k++) {
        t->x[k] = s->x[k]; t->y[k] = s->y[k]; t->rgb[k] = s->rgb[k];
        t->u[k] = s->u[k]; t->v[k] = s->v[k];
    }
    t->clut = s->clut; t->tpage = s->tpage;
    t->textured = s->textured; t->gouraud = s->gouraud; t->quad = s->quad;
    memcpy(t->raw, s->raw, sizeof(t->raw));
    t->nraw = s->nraw;
    s_gte_n++;
    return 1;
}

/** @brief Walk the prims the emitter wrote to the polybuf in [start, end) into s_gte.
 *
 * A tag longer than 24 words ends the walk; at most MAX_PRIMS prims are
 * stored, 20 raw words each. Stored prims are counted by code byte into
 * s_code_hist, which accumulates across calls.
 * @param cpu The guest CPU state, read only.
 * @param start First polybuf address of the emitter's output.
 * @param end One past the last. */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    OP_PROF_ACCUM_SCOPE("flame.capture");
    s_gte_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 24, .raw_words_max = 20, .guard = MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = NULL,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, flame_prim_sink, NULL);
}

/* ---- one-shot input + prim dump ------------------------------------------ */
/** @brief Print the emitter's inputs and the first captured prims to stderr.
 *
 * Prints the flame origin, the active and super words, the per-strand
 * counters and lengths, the attach matrix, the camera projection matrix and
 * position, the first geometry records, the cap colours, the connection
 * table, the strip colour/UV words, the first 16 spine records of each live
 * strand, the prim code histogram, and the first 6 captured prims raw and
 * decoded. Reads s_gte, so capture_prims runs first.
 * @param cpu The guest CPU state, read only.
 * @param start First polybuf address of the emitter's output.
 * @param end One past the last. */
static void flame_dump(CPUState* cpu, uint32_t start, uint32_t end) {
    uint32_t fb = FL_STATE;
    fprintf(stderr, "\n=== [flame-dump f=%u] polybuf [%08X,%08X) %u bytes ===\n",
            openpete_frame, start, end, end - start);
    fprintf(stderr, "  m_Position = (%d, %d, %d)\n",
            (int)cpu->read_word(fb+FL_POS), (int)cpu->read_word(fb+FL_POS+4),
            (int)cpu->read_word(fb+FL_POS+8));
    fprintf(stderr, "  m_IsFlameActive=%d  super=%d\n",
            (int)cpu->read_word(fb+FL_ACTIVE), flame_super(cpu));
    fprintf(stderr, "  per-strand cnt[0..7] =");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %d", (int8_t)cpu->read_byte(fb+FL_CNT+(uint32_t)i));
    fprintf(stderr, "\n  per-strand len[0..7] =");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %d", (int8_t)cpu->read_byte(fb+FL_LEN+(uint32_t)i));
    fprintf(stderr, "\n  attach matrix (+%02X, 5 words -> 9 int16):", FL.attach_off);
    for (int i = 0; i < 5; i++) fprintf(stderr, " %08X", cpu->read_word(fb+FL.attach_off+(uint32_t)i*4));
    fprintf(stderr, "\n  camera projMat (+0x00, 5 words):");
    for (int i = 0; i < 5; i++) fprintf(stderr, " %08X", cpu->read_word(game_layout.cam.base+(uint32_t)i*4));
    fprintf(stderr, "\n  camera pos (%08X) = (%d, %d, %d)\n", game_layout.cam.position,
            (int)cpu->read_word(game_layout.cam.position),
            (int)cpu->read_word(game_layout.cam.position + 4),
            (int)cpu->read_word(game_layout.cam.position + 8));

    fprintf(stderr, "  geom[0..3] (16B records):\n");
    for (int r = 0; r < 4; r++)
        fprintf(stderr, "    +%02X: %08X %08X %08X %08X\n", r*16,
                cpu->read_word(FL.geom+(uint32_t)r*16+0), cpu->read_word(FL.geom+(uint32_t)r*16+4),
                cpu->read_word(FL.geom+(uint32_t)r*16+8), cpu->read_word(FL.geom+(uint32_t)r*16+12));
    fprintf(stderr, "  cap colours[0..7]:");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", cpu->read_word(FL.cap_colours+(uint32_t)i*4));
    fprintf(stderr, "\n  conn[0..7]:");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", cpu->read_word(FL.conn+(uint32_t)i*4));
    fprintf(stderr, "\n  flame struct +0x08..0x1C (strip colour/UV base):");
    for (int o = 0x08; o <= 0x1C; o += 4) fprintf(stderr, " +%02X=%08X", o, cpu->read_word(fb+(uint32_t)o));
    fprintf(stderr, "\n");

    /* per active-strand geometry block. Dump the first 16 8-byte verts. */
    for (int strand = 0; strand < 8; strand++) {
        int cnt = (int8_t)cpu->read_byte(fb+FL_CNT+(uint32_t)strand);
        if (cnt <= 0) continue;
        int len = (int8_t)cpu->read_byte(fb+FL_LEN+(uint32_t)strand);
        uint32_t conn = cpu->read_word(FL.conn + (uint32_t)(cnt-1)*4
                                       + (strand >= 4 ? FL.conn_hi_off : 0u));
        uint32_t off = (strand < 4) ? FL.geom_stride_lo*(uint32_t)strand
                                    : FL.geom_stride_hi*(uint32_t)(strand-4) + FL.geom_hi_base;
        uint32_t s6 = FL.geom + off;
        uint32_t s7off = ((conn >> 24) << 3) & 0x7F8u;
        fprintf(stderr, "  strand %d: cnt=%d len=%d conn=%08X off=%X s6=%08X s7=s6+%X verts(8B Z/X/Y):\n",
                strand, cnt, len, conn, off, s6, s7off);
        for (int r = 0; r < 16; r++) {
            uint32_t w0 = cpu->read_word(s6+(uint32_t)r*8), w1 = cpu->read_word(s6+(uint32_t)r*8+4);
            int Z = (int16_t)(w0 & 0xFFFF), X = -(int)(int16_t)(w0 >> 16), Y = -(int)(int16_t)(w1 & 0xFFFF);
            fprintf(stderr, "    [%2d] %08X %08X  Z=%d X=%d Y=%d scale=%04X\n", r, w0, w1, Z, X, Y, w1 >> 16);
        }
    }

    /* code histogram */
    fprintf(stderr, "  prim code histogram:");
    for (int c = 0; c < 256; c++) if (s_code_hist[c]) fprintf(stderr, " %02X:%u", c, s_code_hist[c]);
    fprintf(stderr, "  (total prims=%d)\n", s_gte_n);

    /* first few prims, raw + decoded */
    int n = s_gte_n < 6 ? s_gte_n : 6;
    for (int p = 0; p < n; p++) {
        prim_t* t = &s_gte[p];
        fprintf(stderr, "  prim[%d] code=%02X len=%d nv=%d tex=%d gour=%d quad=%d  raw:",
                p, t->code, t->len, t->nv, t->textured, t->gouraud, t->quad);
        for (int i = 0; i < t->nraw; i++) fprintf(stderr, " %08X", t->raw[i]);
        fprintf(stderr, "\n      decoded:");
        for (int k = 0; k < t->nv; k++)
            fprintf(stderr, " v%d(xy=%.0f,%.0f rgb=%06X uv=%d,%d)",
                    k, t->x[k], t->y[k], t->rgb[k], t->u[k], t->v[k]);
        if (t->textured) fprintf(stderr, "  clut=%04X tpage=%04X", t->clut, t->tpage);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "=== end flame-dump ===\n\n");
}

/** @brief Print one stderr line summarizing the captured prims.
 *
 * The line holds the prim count split into quads, triangles and textured
 * prims, the super flag, and the flame origin. The hook calls it for at most
 * 200 verify reports.
 * @param cpu The guest CPU state, read only. */
static void flame_report(CPUState* cpu) {
    uint32_t fb = FL_STATE;
    int textured = 0, quad = 0, tri = 0;
    for (int p = 0; p < s_gte_n; p++) {
        if (s_gte[p].textured) textured++;
        if (s_gte[p].nv == 4) quad++; else if (s_gte[p].nv == 3) tri++;
    }
    fprintf(stderr, "[flame f=%u] prims=%d (quad=%d tri=%d tex=%d) super=%d pos=(%d,%d,%d)\n",
            openpete_frame, s_gte_n, quad, tri, textured, flame_super(cpu),
            (int)cpu->read_word(fb+FL_POS), (int)cpu->read_word(fb+FL_POS+4), (int)cpu->read_word(fb+FL_POS+8));
}

/* ---- flame origin -> camera space (two-stage transform, step 1) ---------- */
/** @brief Truncate an int to its low 16 bits, sign-extended, as mtc2 does on a GTE IR load.
 *
 * @param v The value.
 * @return The sign-extended low halfword. */
static int s16(int v) { return (int)(int16_t)(uint32_t)v; }

/** @brief Compute the flame origin's camera-space position, the translation of every flame vertex.
 *
 * Mirrors r_flame.s lines 37-70: a swizzled MVMVA (sf=1, mx=RT, vv=IR, tv=none)
 * of the flame origin minus the integer camera position through the camera
 * rotation matrix. The IR vector is swizzled as the avatar's TR_body is:
 * IR1 = cam.y - flame.y, IR2 = cam.z - flame.z, IR3 = flame.x - cam.x, each
 * truncated to int16 as mtc2 does. The Q12 result is divided by 4096.0 without
 * truncation, so the origin follows the sub-tick camera. The attach matrix then
 * replaces the rotation for the vertex projection.
 * @param cpu The guest CPU state, read only.
 * @param v The projection view (integer camera position and rotation).
 * @param tr Receives the camera-space translation. */
static void flame_origin_tr(CPUState* cpu, const ngte_view* v, double tr[3]) {
    int fx = (int)cpu->read_word(FL_STATE + FL_POS + 0);
    int fy = (int)cpu->read_word(FL_STATE + FL_POS + 4);
    int fz = (int)cpu->read_word(FL_STATE + FL_POS + 8);
    int cx = (int)v->cam_ram[0];   /* integer camera, as the .s loads it */
    int cy = (int)v->cam_ram[1];
    int cz = (int)v->cam_ram[2];
    int ir1 = s16(cy - fy);   /* C2_IR1 */
    int ir2 = s16(cz - fz);   /* C2_IR2 */
    int ir3 = s16(fx - cx);   /* C2_IR3 */
    const int (*R)[3] = v->cam_M;
    /* Fractional matrix-scale (÷4096.0, not >>12) so the ribbon's camera-space
     * origin tracks the smooth sub-tick camera instead of stepping ~1 view-unit
     * per present (same fix as the moby/terrain deltas; the s16 IR delta above is
     * the faithful GTE int16 load and the dominant loss was this final truncation). */
    for (int i = 0; i < 3; i++)
        tr[i] = (R[i][0]*ir1 + R[i][1]*ir2 + R[i][2]*ir3) / 4096.0;
}

/* ---- native gamestate walk (float geometry for HD + frame-gen) ------------
 * Re-derive the flame ribbon from gamestate in float: reproduces the procedural
 * generation (spine curve + sine flicker + ribbon assembly) and projects in
 * float, so the geometry carries sub-pixel/HD precision and sub-tick
 * interpolation. The ribbon is the STRIP (GT4 textured additive) plus the
 * first-cell caps (G3). The VERIFY path compares the SXY shape against the
 * captured prims (the .s int16 output as the oracle). */
/** @brief Read the signed halfword at a halfword-aligned guest address through a word read.
 *
 * @param cpu The guest CPU state, read only.
 * @param addr The address; bit 1 selects the high half of the aligned word.
 * @return The sign-extended halfword. */
static int rd_s16(CPUState* cpu, uint32_t addr) {
    uint32_t w = cpu->read_word(addr & ~3u);
    return (int)(int16_t)((addr & 2u) ? (w >> 16) : (w & 0xFFFF));
}

/** @brief Load a PsyQ rotation matrix (5 words holding 9 int16, 1.0 = 4096) into R.
 *
 * @param cpu The guest CPU state, read only.
 * @param base Guest address of the matrix.
 * @param R Receives the matrix. */
static void flame_load_matrix(CPUState* cpu, uint32_t base, int R[3][3]) {
    ngte_read_matrix(cpu, base, R);
}

/** @brief Project one local vertex through R and TR by the GTE RTPS math, in float.
 *
 * Uses ngte_project_gte with the +-1024 SXY bound on.
 * @param R The Q12 rotation (the flame attach matrix).
 * @param TR The camera-space translation of the flame origin.
 * @param ofx Screen X offset.
 * @param ofy Screen Y offset.
 * @param v The projection view.
 * @param vx Local X.
 * @param vy Local Y.
 * @param vz Local Z.
 * @param sx Receives screen X.
 * @param sy Receives screen Y.
 * @param sz Receives view-space Z (MAC3). */
static void flame_project(const int R[3][3], const double TR[3], double ofx, double ofy, const ngte_view* v,
                          double vx, double vy, double vz, float* sx, float* sy, float* sz) {
    ngte_proj o;   /* shared GTE-faithful pipeline, ±1024 SXY bound ON */
    ngte_project_gte(R, TR, vx, vy, vz, ofx, ofy, v, 1, &o);
    *sx = (float)o.sx; *sy = (float)o.sy; *sz = (float)o.m3;
}

/** @brief Read one spine cross-section record, lerped toward the next record by the sub-keyframe slide.
 *
 * A record is 8 bytes: X in the high and Z in the low half of word 0, the
 * vertical offset in the low half of word 1, and the sine-table index (even,
 * 0..0x1FE) in bits 16-23 of word 1 shifted left by 1. The counter advance
 * moves the window by one record per step, so record p+8 is where the next
 * integer counter reads this cross-section, and a slide of 1 converges on it.
 * X, Z and the offset lerp rounded to nearest; the sine index lerps along the
 * shorter way round its 512-entry wrap. The lerp is skipped when Z goes
 * backward or jumps by 200 or more, where the read has crossed into the next
 * strand's spine at maximum extension.
 * @param cpu The guest CPU state, read only.
 * @param p Guest address of the record.
 * @param Xraw Receives X.
 * @param Z Receives Z.
 * @param idx Receives the vertical offset.
 * @param sIdx Receives the sine-table byte index. */
static void flame_read_cross(CPUState* cpu, uint32_t p,
                             int* Xraw, int* Z, int* idx, int* sIdx) {
    uint32_t w0 = cpu->read_word(p), w1 = cpu->read_word(p + 4);
    int X0 = s16(w0 >> 16), Z0 = s16(w0 & 0xFFFF), i0 = s16(w1 & 0xFFFF);
    int s0 = (int)((w1 >> 15) & 0x1FEu);
    double f = s_flame_subframe;
    if (f > 0.0) {
        uint32_t w0b = cpu->read_word(p + 8), w1b = cpu->read_word(p + 12);
        int X1 = s16(w0b >> 16), Z1 = s16(w0b & 0xFFFF), i1 = s16(w1b & 0xFFFF);
        int s1 = (int)((w1b >> 15) & 0x1FEu);
        int dZ = Z1 - Z0;
        if (dZ >= 0 && dZ < 200) {          /* continuous spine - lerp */
            X0 += (int)((X1 - X0) * f + (X1 >= X0 ? 0.5 : -0.5));
            Z0 += (int)(dZ * f + 0.5);
            i0 += (int)((i1 - i0) * f + (i1 >= i0 ? 0.5 : -0.5));
            int ds = s1 - s0; if (ds > 256) ds -= 512; else if (ds < -256) ds += 512;
            s0 = (s0 + (int)(ds * f + (ds >= 0 ? 0.5 : -0.5))) & 0x1FE;
        }   /* else: discontinuity (crossed into next strand) - keep record[p] */
    }
    *Xraw = X0; *Z = Z0; *idx = i0; *sIdx = s0;
}

typedef struct { float sx, sy, sz; } fpt;
/** @brief Project one ribbon cross-section: the spine record at p becomes two edge vertices.
 *
 * The record is read with flame_read_cross (sub-keyframe slide applied). The
 * sine-table entries at its index and a quarter turn later (+0x80 bytes) scale
 * by amp (>> 11) into a cross offset (co, si); the edges are
 * (co - X, si - idx) and (-co - X, -si - idx), both at the record's Z, and
 * each goes through flame_project.
 * @param cpu The guest CPU state, read only.
 * @param p Guest address of the spine record.
 * @param amp The ribbon half-width amplitude.
 * @param R The attach matrix.
 * @param TR The camera-space flame origin.
 * @param ofx Screen X offset.
 * @param ofy Screen Y offset.
 * @param v The projection view.
 * @param e0 Receives the first edge vertex.
 * @param e1 Receives the second edge vertex. */
static void flame_cross_section(CPUState* cpu, uint32_t p, int amp,
                                const int R[3][3], const double TR[3],
                                double ofx, double ofy, const ngte_view* v, fpt* e0, fpt* e1) {
    int Xraw, Z, idx, sIdx;
    flame_read_cross(cpu, p, &Xraw, &Z, &idx, &sIdx);
    int cosv = rd_s16(cpu, game_layout.sin_table + (uint32_t)sIdx + 0x80);
    int sinv = rd_s16(cpu, game_layout.sin_table + (uint32_t)sIdx);
    int co = (amp * cosv) >> 11;
    int si = (amp * sinv) >> 11;
    /* VXY0 = (co - Xraw, si - idx); VXY1 = (-co - Xraw, -si - idx); both at Z */
    flame_project(R, TR, ofx, ofy, v, (double)(co - Xraw), (double)(si - idx), (double)Z,
                  &e0->sx, &e0->sy, &e0->sz);
    flame_project(R, TR, ofx, ofy, v, (double)(-co - Xraw), (double)(-si - idx), (double)Z,
                  &e1->sx, &e1->sy, &e1->sz);
}

/* native ribbon quads (for A/B vs the captured GT4 prims + the scene feed) */
typedef struct {
    float sx[4], sy[4], sz[4];
    int   u[4], v[4];
    uint32_t rgb[4];
    int   tpx, tpy, clx, cly, bpp;
} fquad;
#define MAX_FQUAD 1024
static fquad s_fquad[MAX_FQUAD]; static int s_fquad_n;

/* native cap tris (G3 untextured opaque) - the first-cell flame base */
typedef struct { float sx[3], sy[3], sz[3]; uint32_t rgb[3]; } fcap;
#define MAX_FCAP 1024
static fcap s_fcap[MAX_FCAP]; static int s_fcap_n;

/* strands whose counter was live on the last walk - the census's engagement
 * count, separating "no flame this present" from "a flame that emitted none". */
static int s_strands_live;

/** @brief Signed twice-area of a cap triangle, with the sign of the GTE NCLIP MAC0.
 *
 * The emitter emits a cap only when MAC0 > 0. The GTE runs NCLIP on the int16
 * SXY, so the corners are rounded to the integer grid before the product;
 * otherwise near-degenerate caps cull differently from the emitter.
 * @param a First corner.
 * @param b Second corner.
 * @param c Third corner.
 * @return The signed twice-area on the integer grid. */
static double cap_nclip(const fpt* a, const fpt* b, const fpt* c) {
    return ngte_nclip(a->sx, a->sy, b->sx, b->sy, c->sx, c->sy, NGTE_NCLIP_SXY_GRID);
}
/** @brief Append a cap triangle to s_fcap when its NCLIP is positive.
 *
 * Drops the triangle when cap_nclip is not above 0 (the emitter's blez on
 * MAC0) or when s_fcap holds MAX_FCAP caps. Colours keep their low 24 bits.
 * @param a First corner.
 * @param b Second corner.
 * @param c Third corner.
 * @param ca Colour word of the first corner.
 * @param cb Colour word of the second corner.
 * @param cc Colour word of the third corner. */
static void emit_cap(const fpt* a, const fpt* b, const fpt* c,
                     uint32_t ca, uint32_t cb, uint32_t cc) {
    if (cap_nclip(a, b, c) <= 0.0) return;          /* blez MAC0 -> drop */
    if (s_fcap_n >= MAX_FCAP) return;
    fcap* t = &s_fcap[s_fcap_n++];
    t->sx[0]=a->sx; t->sy[0]=a->sy; t->sz[0]=a->sz; t->rgb[0]=ca & 0xFFFFFFu;
    t->sx[1]=b->sx; t->sy[1]=b->sy; t->sz[1]=b->sz; t->rgb[1]=cb & 0xFFFFFFu;
    t->sx[2]=c->sx; t->sy[2]=c->sy; t->sz[2]=c->sz; t->rgb[2]=cc & 0xFFFFFFu;
}

/** @brief Emit the flame-base caps of one strand's first cell (r_flame.s block_80058F2C..8005920C).
 *
 * Projects the spine centre at s7 without flicker (t5), then three flicker
 * vertices from the record at s7-16 (e0, e1, e2) with the cap amplitude
 * (the super amplitude in the super form, where e0's offsets are also
 * halved), and emits four NCLIP-gated G3 caps
 * combining them with the four cap colours.
 * @param cpu The guest CPU state, read only.
 * @param s7 Guest address of the strand's first spine record.
 * @param super Nonzero in the wider second form.
 * @param R The attach matrix.
 * @param TR The camera-space flame origin.
 * @param ofx Screen X offset.
 * @param ofy Screen Y offset.
 * @param v The projection view.
 * @return 1 when the centre is near-culled ((SZ >> 7) - 2 < 0, clamped to
 *         0..0xFFFF), in which case the emitter skips the whole strand, caps
 *         and strip; else 0. */
static int flame_first_cell(CPUState* cpu, uint32_t s7, int super,
                            const int R[3][3], const double TR[3],
                            double ofx, double ofy, const ngte_view* v) {
    uint32_t w0c = cpu->read_word(s7), w1c = cpu->read_word(s7 + 4);
    int Xc = s16(w0c >> 16), Zc = s16(w0c & 0xFFFF), idxc = s16(w1c & 0xFFFF);
    fpt t5;  /* spine centre (no flicker) */
    flame_project(R, TR, ofx, ofy, v, (double)(-Xc), (double)(-idxc), (double)Zc,
                  &t5.sx, &t5.sy, &t5.sz);
    int sz = (int)t5.sz; if (sz < 0) sz = 0; else if (sz > 0xFFFF) sz = 0xFFFF;
    if (((sz >> 7) - 2) < 0) return 1;              /* near-cull -> skip strand */

    uint32_t w0 = cpu->read_word(s7 - 16), w1 = cpu->read_word(s7 - 12);
    int Xraw2 = s16(w0 >> 16), Z2 = s16(w0 & 0xFFFF), idx2 = s16(w1 & 0xFFFF);
    int sIdx2 = (int)((w1 >> 15) & 0x1FEu);
    int amp = super ? FL.cap_amp_super : FL.cap_amp;
    int co = (amp * rd_s16(cpu, game_layout.sin_table + (uint32_t)sIdx2 + 0x80)) >> 11;
    int si = (amp * rd_s16(cpu, game_layout.sin_table + (uint32_t)sIdx2)) >> 11;
    int t0 = -co, t1 = si;
    if (super) { t0 >>= 1; t1 >>= 1; }
    fpt e0, e1, e2;
    flame_project(R, TR, ofx, ofy, v, (double)(t1 - Xraw2), (double)(t0 - idx2), (double)Z2,
                  &e0.sx, &e0.sy, &e0.sz);                       /* VXY0 */
    flame_project(R, TR, ofx, ofy, v, (double)(co - Xraw2), (double)(si - idx2), (double)Z2,
                  &e1.sx, &e1.sy, &e1.sz);                       /* VXY1 */
    flame_project(R, TR, ofx, ofy, v, (double)(-co - Xraw2), (double)(-si - idx2), (double)Z2,
                  &e2.sx, &e2.sy, &e2.sz);                       /* VXY2 */

    uint32_t cb = FL.cap_colours + (FL.cap_colour_off ? FL.cap_colour_off(cpu) : 0u);
    uint32_t c0 = cpu->read_word(cb), c1 = cpu->read_word(cb + 4);
    uint32_t c2 = cpu->read_word(cb + 8), c3 = cpu->read_word(cb + 12);
    emit_cap(&t5, &e1, &e0, c0, c2, c1);             /* A: (t5,t7,t6) */
    emit_cap(&t5, &e0, &e2, c0, c1, c3);             /* B: (t5,t6,s0) */
    emit_cap(&t5, &e2, &e1, c0, c3, c2);             /* C: (t5,s0,t7) */
    emit_cap(&e0, &e1, &e2, c1, c2, c3);             /* D: (t6,t7,s0) */
    return 0;
}

/** @brief Rebuild the native ribbon quads (s_fquad) and caps (s_fcap) from the flame state block.
 *
 * Mirrors the emitter, r_flame.s: resets the counts, builds the projection
 * view, the attach matrix and the camera-space origin, then walks strands 7
 * down to 0, skipping those whose counter byte is not positive. Per strand the
 * connection word selects the first spine record and the counter and length
 * bytes bound the window. When the window bounds hold, flame_first_cell emits
 * the base caps and can skip the strand on a near cull. The strip then steps
 * back 16 bytes per cross-section, projecting each with flame_cross_section
 * (the amplitude is 8 while the counter is spent or the .s gp accumulator is
 * 0, which holds on the first cross-section when the cap bounds failed), and
 * appends a textured quad per
 * consecutive pair with a UV ramp and a colour ramp that darkens by step.
 * The counter drops by 2 per pass. Logs an error when the strip tpage's blend
 * mode is not additive, since the feed hardcodes additive. Output stops at
 * MAX_FQUAD quads and MAX_FCAP caps; s_strands_live counts strands walked.
 * The first three walks print a debug line when OPENPETE_FLAME_DEBUG is set.
 * Reads guest RAM only.
 * @param cpu The guest CPU state, read only. */
static void flame_build_strip(CPUState* cpu) {
    s_fquad_n = 0;
    s_fcap_n = 0;
    s_strands_live = 0;
    uint32_t fb = FL_STATE;
    int super = flame_super(cpu);
    int R[3][3]; double TR[3];
    ngte_view view; ngte_view_build(cpu, &view);   /* one projection view per walk */
    const ngte_view* v = &view;
    flame_load_matrix(cpu, fb + FL.attach_off, R);
    flame_origin_tr(cpu, v, TR);
    double ofx = v->ofx, ofy = v->ofy;

    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_FLAME_DEBUG"); dbg = (e && *e && *e != '0'); }
    static int dbg_n = 0;
    int do_dbg = dbg && dbg_n < 3;

    for (int strand = 7; strand >= 0; strand--) {   /* .s processes t9 = 7..0 */
        int cnt = (int8_t)cpu->read_byte(fb + FL_CNT + (uint32_t)strand);
        if (cnt <= 0) continue;
        s_strands_live++;
        int len = (int8_t)cpu->read_byte(fb + FL_LEN + (uint32_t)strand);
        uint32_t connPtr = FL.conn + (uint32_t)(cnt - 1) * 4;
        uint32_t stride;
        if (strand >= 4) { connPtr += FL.conn_hi_off; stride = FL.geom_stride_hi; }
        else             { stride = FL.geom_stride_lo; }
        uint32_t conn = cpu->read_word(connPtr);
        uint32_t geomOff = (strand < 4)
            ? FL.geom_stride_lo * (uint32_t)strand
            : FL.geom_stride_hi * (uint32_t)(strand - 4) + FL.geom_hi_base;
        uint32_t s6 = FL.geom + geomOff;
        uint32_t s7 = s6 + (((conn >> 24) << 3) & 0x7F8u);
        uint32_t s5b = s6 + (uint32_t)len * 8u;
        uint32_t v1b = s6 + stride;
        /* First-cell bounds (line 125-128): these gate the CAPS only - on
         * failure the .s sets gp=0 and jumps to .L8005920C, which still runs
         * the strip (just with the amplitude forced to 8). So never skip the
         * whole strand here; only the caps. (Skipping the strand dropped the
         * flame's retract phase, where len shrinks and s7 > s5b.) */
        int gp = (!((int32_t)(s7 - s5b) > 0) && !((int32_t)(v1b - s7) <= 0)) ? 1 : 0;

        if (gp) {                                    /* first cell: the base caps */
            if (flame_first_cell(cpu, s7, super, R, TR, ofx, ofy, v))
                continue;                            /* near-cull -> skip strand */
        }
        s7 -= 0x10;                                  /* .L8005920C */
        int byte1 = (int)((conn >> 8) & 0xFF);
        int byte2 = (int)((conn >> 16) & 0xFF);
        if ((int32_t)(s7 - s5b) > 0) {               /* clamp to s5b */
            int d = (int)((s7 - s5b) >> 3);
            byte1 += d; byte2 -= d; s7 = s5b;
        }
        int strand_q0 = s_fquad_n, byte2_init = byte2;
        int grey = 0x80 - 5 * byte1; if (grey < 0) grey = 0;
        uint32_t color = ((uint32_t)grey) | ((uint32_t)grey << 8) | ((uint32_t)grey << 16);
        uint32_t base = super ? fb + 8 : fb;
        uint32_t uv0w = cpu->read_word(base + 0x10) + ((uint32_t)byte1 << 8);
        uint32_t uv1w = cpu->read_word(base + 0x14) + (uint32_t)FL.uv1_bias
                        + ((uint32_t)byte1 << 8);
        int tpx = ((int)((uv1w >> 16) & 0xF)) * 64, tpy = ((int)((uv1w >> 20) & 1)) * 256;
        int mode = (int)((uv1w >> 23) & 3), bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
        /* ABR-FIXME: the flame-strip draw hardcodes blend=2 (ABR1
         * additive); the strip tpage's ABR is data-driven (assumed additive, not
         * statically proven). If a flame ever ships a non-additive tpage this fires -
         * derive blend from the captured ABR instead of hardcoding. */
        if (((uv1w >> 21) & 3) != 1)
            OPL_ERROR_F("native", "ABR-FIXME flame strip tpage ABR=%d (feeder hardcodes "
                        "blend=2 additive) f=%u",
                        (int)((uv1w >> 21) & 3), openpete_frame);
        int clx = ((int)((uv0w >> 16) & 0x3F)) * 16, cly = (int)((uv0w >> 22) & 0x1FF);
        uint32_t s4 = 0xA00u, s5step = 0x0A0A0Au;

        fpt prev0, prev1; int have_prev = 0;
        uint32_t puv0 = uv0w, puv1 = uv1w, pcol = color;
        /* `gp` is NOT a constant gate: line 386 `add gp,gp,at` accumulates the
         * previous cross-section's packed SXY0 each step (at=0 only on the first
         * step, line 361). So gp==0 (amp forced to 8) holds only for the FIRST
         * cross-section when the caps-bounds failed; after one step gp is the
         * SXY sum (non-zero) and amp reverts to the strip amplitude. Reproduce
         * the accumulator exactly or the retract phase (gp=0 strands) renders
         * too narrow. */
        int32_t gp_acc = gp;
        int32_t prev_sxy0 = 0;          /* `at` on loop entry (.s line 361) */
        for (;;) {
            gp_acc += prev_sxy0;        /* add gp, gp, at */
            int amp = (byte2 <= 0 || gp_acc == 0) ? 8
                                                  : (super ? FL.strip_amp_super : FL.strip_amp);
            fpt c0, c1;
            flame_cross_section(cpu, s7, amp, R, TR, ofx, ofy, v, &c0, &c1);
            s7 -= 0x10;
            /* pack edge0's SXY (the GTE SXY0 the .s reads into `at`) for next +gp */
            prev_sxy0 = (int32_t)(((uint32_t)(uint16_t)(int16_t)lrintf(c0.sx))
                                  | ((uint32_t)(int16_t)lrintf(c0.sy) << 16));
            if (have_prev && s_fquad_n < MAX_FQUAD) {
                fquad* q = &s_fquad[s_fquad_n++];
                /* GT4 order: v0=prevEdge0 v1=prevEdge1 v2=curEdge0 v3=curEdge1 */
                fpt vv[4] = { prev0, prev1, c0, c1 };
                uint32_t cuv0 = puv0 + s4, cuv1 = puv1 + s4, ncol = pcol - s5step;
                uint32_t uvw[4] = { puv0, puv1, cuv0, cuv1 };
                uint32_t cols[4] = { pcol, pcol, ncol, ncol };
                for (int k = 0; k < 4; k++) {
                    q->sx[k] = vv[k].sx; q->sy[k] = vv[k].sy; q->sz[k] = vv[k].sz;
                    q->u[k] = (int)(uvw[k] & 0xFF); q->v[k] = (int)((uvw[k] >> 8) & 0xFF);
                    q->rgb[k] = cols[k] & 0xFFFFFFu;
                }
                q->tpx = tpx; q->tpy = tpy; q->clx = clx; q->cly = cly; q->bpp = bpp;
                /* advance the ramp (carry into next quad) */
                puv0 += s4; puv1 += s4; pcol -= s5step;
            }
            prev0 = c0; prev1 = c1; have_prev = 1;
            /* counter (.L80059298): the bgtz AND the blez both carry an
             * `addi t8,-1` in their branch-delay slot, and a delay slot runs
             * whether or not its branch is taken - so EACH counter pass drops
             * byte2 by 2, not 1. (Getting this wrong doubled the quad count.)
             * The s7+=8 / s4,s5 final-step swap happens only on the fall-through
             * (the second test failed). */
            if (byte2 <= 0) break;        /* blez t8 (old value) */
            byte2 -= 1;                   /* delay slot of blez */
            int last = byte2;             /* value the bgtz tests */
            byte2 -= 1;                   /* delay slot of bgtz (always runs) */
            if (last <= 0) { s7 += 8; s4 = 0x500u; s5step = 0x050505u; }
        }
        if (do_dbg)
            fprintf(stderr, "    [flame-dbg] strand %d cnt=%d conn=%08X byte1=%d byte2_init=%d quads=%d\n",
                    strand, cnt, conn, byte1, byte2_init, s_fquad_n - strand_q0);
    }
    if (do_dbg) { fprintf(stderr, "    [flame-dbg] total native quads=%d (frame %u)\n", s_fquad_n, openpete_frame); dbg_n++; }
}

/** @brief Compare the native ribbon quads and caps against the captured GT4 and G3 prims and print the result.
 *
 * Rebuilds the native ribbon, then pairs native quads with captured textured
 * quads element-wise (the walk visits strands and steps in the emitter's
 * order, and the GT4 vertex order matches), and native caps with captured
 * untextured triangles likewise. Per pair the vertex distance is |dx| + |dy|;
 * each line prints both counts, the mean distance, the worst pair's maximum,
 * and the number of pairs whose maximum exceeds 4 pixels. The emitter's
 * output is integer-quantized, so about 1 pixel is expected.
 * @param cpu The guest CPU state, read only. */
static void flame_strip_compare(CPUState* cpu) {
    flame_build_strip(cpu);
    int gt4 = 0; for (int p = 0; p < s_gte_n; p++) if (s_gte[p].textured && s_gte[p].nv == 4) gt4++;
    int n = s_fquad_n < gt4 ? s_fquad_n : gt4;
    double worst = 0, sum = 0; int worst_q = -1, over4 = 0, nv = 0;
    int qi = 0;
    for (int p = 0; p < s_gte_n && qi < n; p++) {
        prim_t* pr = &s_gte[p];
        if (!(pr->textured && pr->nv == 4)) continue;
        fquad* q = &s_fquad[qi];
        double qworst = 0;
        for (int k = 0; k < 4; k++) {
            double dx = q->sx[k] - pr->x[k], dy = q->sy[k] - pr->y[k];
            double d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            sum += d; nv++;
            if (d > qworst) qworst = d;
        }
        if (qworst > worst) { worst = qworst; worst_q = qi; }
        if (qworst > 4.0) over4++;
        qi++;
    }
    fprintf(stderr, "[flame-strip f=%u] quads native=%d prims(GT4)=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx @q%d over4px=%d/%d\n",
            openpete_frame, s_fquad_n, gt4, n, nv ? sum/nv : 0, worst, worst_q, over4, n);

    /* caps A/B: native G3 caps vs the captured untextured G3 prims (code 0x30) */
    int g3 = 0; for (int p = 0; p < s_gte_n; p++) if (!s_gte[p].textured && s_gte[p].nv == 3) g3++;
    int cn = s_fcap_n < g3 ? s_fcap_n : g3;
    double cworst = 0, csum = 0; int cover4 = 0, cnv = 0, ci = 0;
    for (int p = 0; p < s_gte_n && ci < cn; p++) {
        prim_t* pr = &s_gte[p];
        if (!(!pr->textured && pr->nv == 3)) continue;
        fcap* t = &s_fcap[ci];
        double tw = 0;
        for (int k = 0; k < 3; k++) {
            double dx = t->sx[k] - pr->x[k], dy = t->sy[k] - pr->y[k];
            double d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            csum += d; cnv++; if (d > tw) tw = d;
        }
        if (tw > cworst) cworst = tw;
        if (tw > 4.0) cover4++;
        ci++;
    }
    fprintf(stderr, "[flame-cap   f=%u] caps native=%d prims(G3)=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx over4px=%d/%d\n",
            openpete_frame, s_fcap_n, g3, cn, cnv ? csum/cnv : 0, cworst, cover4, cn);
}

/** @brief Whether the current VBL lies in the OPENPETE_FLAME_CENSUS range.
 *
 * The variable is LO-HI or a single VBL. While active, each feed prints the
 * walk's engagement counts: strands whose counter was live, ribbon quads,
 * caps that survived NCLIP, and the triangles handed to the scene. A feed
 * with no flame prints zeros, which separates a producer that did not run
 * from one that emitted nothing.
 * @return 1 inside the range, else 0. */
static int flame_census_on(void) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    return env_frame_range_active(&s_range, "OPENPETE_FLAME_CENSUS", openpete_frame);
}
/** @brief Print one flame census line for a feed.
 *
 * @param src Label of the caller, printed in the line.
 * @param slot The model record slot the feed used.
 * @param tris Triangle count handed to the scene. */
static void flame_census_line(const char* src, int slot, int tris) {
    fprintf(stderr, "[flame-census f=%u %s slot=%d] strands=%d quads=%d caps=%d tris=%d\n",
            openpete_frame, src, slot, s_strands_live, s_fquad_n, s_fcap_n, tris);
}

/* ---- native walk feed (float geometry from gamestate) --------------------- */
/** @brief Re-derive the ribbon from gamestate and push it to the native scene.
 *
 * Reads no captured prim, so it runs at the sub-tick gamestate the host
 * re-renders. Each strip quad splits into two textured additive triangles
 * (blend 2) and each cap is an untextured opaque triangle, all in the WORLD
 * group with per-vertex view Z, tagged as the ribbon and caps of `slot`.
 * Nothing is pushed when the walk produced no geometry or the player is
 * hidden (player_hide_active).
 * @param cpu The guest CPU state, read only.
 * @param slot The model record the ribbon renders under; negative is 0.
 * @param src The caller's label in the census line. */
static void flame_feed_native(CPUState* cpu, int slot, const char* src) OP_REQUIRES(sim) {
    flame_build_strip(cpu);                  /* fills s_fquad (strip) + s_fcap (caps) */
    const int cen = flame_census_on();
    if (s_fquad_n <= 0 && s_fcap_n <= 0) {
        if (cen) flame_census_line(src, slot, 0);
        return;
    }
    const int hide = player_hide_active();
    player_hide_count(PH_CAT_FLAME, s_fquad_n * 2 + s_fcap_n, hide);
    if (hide) return;
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);
    const uint32_t tslot = (uint32_t)(slot < 0 ? 0 : slot);   /* the model record it renders under */

    openpete_native_scene_set_tag(OPENPETE_TAG_FLAME(OPENPETE_TAG_FLAME_RIBBON, tslot));
    for (int i = 0; i < s_fquad_n; i++) {
        fquad* q = &s_fquad[i];
        /* GT4 -> two tris: (v0,v1,v2) + (v1,v3,v2) */
        int split[2][3] = { {0,1,2}, {1,3,2} };
        for (int s = 0; s < 2; s++) {
            openpete_scene_tri st = {0};
            for (int k = 0; k < 3; k++) {
                int j = split[s][k];
                st.v[k].sx = q->sx[j]; st.v[k].sy = q->sy[j];
                st.v[k].z  = openpete_native_scene_viewz(openpete_worldvz_full(q->sz[j]));
                st.v[k].pvz = st.v[k].z;   /* unshifted: true view-Z */
                st.v[k].u  = (float)q->u[j]; st.v[k].v = (float)q->v[j];
                st.v[k].rgba = (q->rgb[j] & 0xFFFFFFu) | 0xFF000000u;
            }
            st.textured = 1; st.tpage_x = q->tpx; st.tpage_y = q->tpy;
            st.clut_x = q->clx; st.clut_y = q->cly; st.bpp = q->bpp; st.blend = 2; /* additive */
            openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        }
    }
    openpete_native_scene_set_tag(OPENPETE_TAG_FLAME(OPENPETE_TAG_FLAME_CAPS, tslot));
    for (int i = 0; i < s_fcap_n; i++) {
        fcap* c = &s_fcap[i];
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = c->sx[k]; st.v[k].sy = c->sy[k];
            st.v[k].z  = openpete_native_scene_viewz(openpete_worldvz_full(c->sz[k]));
            st.v[k].pvz = st.v[k].z;   /* unshifted: true view-Z */
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (c->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0; st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 0;                       /* G3 caps are opaque (code 0x30, ABE clear) */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
    openpete_native_scene_set_tag(0);
    if (cen) flame_census_line(src, slot, s_fquad_n * 2 + s_fcap_n);
}

/* ---- gating --------------------------------------------------------------- */
/** @brief Whether OPENPETE_FLAME_VERIFY is set, non-empty and not starting with '0'; read once and cached.
 *
 * @return 1 when the dump and the native-versus-captured comparison are
 *         armed, else 0. */
static int flame_verify_enabled(void) {
    OP_ROLE_ASSERT(sim);
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_FLAME_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
/** @brief First VBL at which the verify dump and comparison run.
 *
 * Read once from OPENPETE_FLAME_VERIFY_FRAME with strtoul (base auto-detected;
 * default 0) and cached.
 * @return The VBL number. */
static unsigned flame_verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_FLAME_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

static hook_bracket_state s_flame_hb;
static const hook_bracket_desc k_flame_bracket = {
    .path = RP_FLAME, .prof_tag = "flame.srend",
    .run = NULL,                      /* the guest entry: the module dispatch runs it */
    .verify_enabled = flame_verify_enabled,
    .st = &s_flame_hb,
};

/** @brief Dispatch hook for the game's flame emitter: capture its prims and feed the native ribbon.
 *
 * Runs the hook bracket, which calls the real emitter once. When the emitter
 * wrote prims (endp > startp) the flame record is appended to the player
 * records and yields the model slot. With the native scene live or verifying
 * the prims are captured; with the scene live and RP_FLAME selected the ribbon
 * is re-derived and pushed under that slot. From flame_verify_frame on, a
 * verifying run dumps the inputs once and prints up to 200 reports and
 * comparisons.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the bracket declined to take over, else 1 (the body has
 *         already run). */
static int on_flame_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_flame_bracket, &startp, &endp, &feed, &verify))
        return 0;
    int slot = 0;
    if (endp > startp) slot = player_records_flame_append(cpu);   /* the flame record, bound to its model record */

    if ((feed || verify) && endp > startp) capture_prims(cpu, startp, endp);

    if (feed && endp > startp && render_path_enabled(RP_FLAME)) {
        flame_feed_native(cpu, slot, "hook");     /* native gamestate walk */
    }

    if (verify && endp > startp && openpete_frame >= flame_verify_frame()) {
        static int s_dumped = 0, s_reports = 0;
        if (!s_dumped) { flame_dump(cpu, startp, endp); s_dumped = 1; }
        if (s_reports < 200) { flame_report(cpu); flame_strip_compare(cpu); s_reports++; }
    }

    return hook_bracket_end(&k_flame_bracket);
}

/* Sub-tick ribbon re-extract; documented at the declaration in flame.h. */
void flame_reextract(CPUState* cpu, int slot) OP_REQUIRES(sim) {
    if (!FL_STATE) return;
    /* Belt-and-braces with the m_IsFlameActive self-gate below: only replay the
     * flame if the canonical frame drew it (skips stale ribbons during a level
     * transition where the active flag could lag the suppressed render). A held
     * cut shows tick N-1's flame records, whose own drawing decides instead. */
    if (!hook_reextract_ready_held(RP_FLAME, player_records_held_pick())) return;
    if (cpu->read_word(FL_STATE + FL_ACTIVE) == 0) return;   /* not breathing fire */
    flame_feed_native(cpu, slot, "rx");
}

/* ---- the sub-tick bracket over one prev/curr pair; documented in flame.h ---- */
int flame_bracket(uint8_t* fl, const uint8_t* pfl, const uint8_t* cfl,
                  double alpha, double* subframe) {
    const int32_t* pp = (const int32_t*)(pfl + FL_POS);
    const int32_t* cp = (const int32_t*)(cfl + FL_POS);
    double dx = (double)cp[0]-pp[0], dy = (double)cp[1]-pp[1], dz = (double)cp[2]-pp[2];
    if (dx*dx + dy*dy + dz*dz > 5000.0 * 5000.0) return 0;
    lerp_vec3  (fl, pfl, cfl, FL_POS, alpha);
    lerp_matrix(fl, pfl, cfl, FL.attach_off, alpha);
    /* Per-strand keyframe counter: advance fractionally so the ribbon
     * extends/scrolls at the present rate. The counter selects the
     * ribbon's window into the static cross-section spine (one 8-byte
     * record per step); write the FLOOR of the interpolated counter as
     * the window and hand the fractional part to the flame feeder as a
     * SUB-KEYFRAME slide (record[p]->record[p+8] lerp) so the ribbon
     * animates below the keyframe grid - as smooth as the avatar's anim
     * lerp, not stepping one record at a time. SNAP on a large jump -
     * the flame periodically reseeds (the counter wraps 30->0), and
     * lerping across that collapses the ribbon through zero. |Δ|<8
     * separates the continuous advance/retract (g_DeltaTime 2..4) from
     * the wrap (~30). The advance is the same for every (non-snapped)
     * strand (all += g_DeltaTime from an integer prev), so the frac is
     * shared; take it from the first advancing strand. */
    for (int st = 0; st < 8; st++) {
        int pcnt = pfl[FL_CNT + st], ccnt = cfl[FL_CNT + st];
        int d = ccnt - pcnt;
        if (d > -8 && d < 8) {
            double fadv = (double)pcnt + (double)d * alpha;
            double ffloor = floor(fadv);
            int v = (int)ffloor; if (v < 0) v = 0;
            fl[FL_CNT + st] = (uint8_t)v;
            if (*subframe == 0.0 && d > 0) *subframe = fadv - ffloor;
        }   /* else keep curr (wrap/reseed - already in fl) */
    }
    return 1;
}

/* Per-record flame pass of a present; documented in flame.h. */
void flame_records_present(CPUState* cpu, int paired, int cam_cut, double alpha, uint8_t* fl) OP_REQUIRES(sim) {
    const uint32_t len = game_layout.player.flame_len;
    if (!FL_STATE || !len || len > PLAYER_REC_AUX_MAX) return;
    double rec_alpha = 0.0;
    int n = player_records_present_begin(paired, cam_cut, alpha, &rec_alpha);
    int nfl = n > 0 ? player_records_flame_count() : 0;
    if (nfl <= 0) { player_records_present_end(); return; }
    uint8_t* flb = fl ? fl : (uint8_t*)openpete_subtick_view_stage(FL_STATE, len);
    if (!flb) flb = (uint8_t*)openpete_psx_to_host_span(FL_STATE, len);
    if (!flb) { player_records_present_end(); return; }
    static uint8_t ambient[PLAYER_REC_AUX_MAX];
    memcpy(ambient, flb, len);
    for (int j = 0; j < nfl; j++) {
        const player_flame_rec_t* d = player_records_flame_curr(j);
        if (!d) break;
        if (!d->drawn) continue;
        const player_flame_rec_t* pd = player_records_flame_prev(j);
        memcpy(flb, d->src, len);
        double sub = 0.0;
        if (player_records_lerps(d->model) && pd) flame_bracket(flb, pd->src, d->src, rec_alpha, &sub);
        flame_native_set_subframe(sub);
        flame_reextract(cpu, d->model);   /* self-gated on the staged m_IsFlameActive */
        player_records_flame_note(j, (const int32_t*)(flb + FL_POS));
    }
    memcpy(flb, ambient, len);
    player_records_present_end();
    flame_native_set_subframe(0.0);   /* the canonical inline render uses exact keyframes */
}

/* The hook address is the layout's flame draw site, bound at boot: a static
 * initializer cannot read another TU's const. A game with no located emitter
 * leaves it 0 and the registry dispatches nothing. */
static feature_dispatch_entry_t flame_dispatch[] = {
    { 0u, on_flame_draw, 0u },
    { 0u, NULL, 0u },
};

/** @brief Bind the flame dispatch entry to the layout's emitter address.
 *
 * A game whose layout locates no emitter leaves the address 0.
 * @param opts Unused. */
static void flame_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    flame_dispatch[0].addr = game_layout.flame.hook;
}

const feature_module_t flame_module = {
    .name           = "flame_native",
    .apply_options  = flame_apply_options,
    .dispatch_table = flame_dispatch,
};
