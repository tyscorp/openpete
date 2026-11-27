// games/spyro-1/renderer/particle.c - the PARTICLE renderer family
// (func_800573C8 particles + func_800580F4 glows + func_800584C4 sparkles /
// r_particles.s) re-derived from gamestate in float + its numerical A/B
// harness. It covers 7 particle sprite classes (FT4 billboards, dots/tiles,
// F2 lines), additive glow rings, and twinkle sparkle quads - a mix of GPU
// primitive types over one polybuf.
//
// The .s writes Particle.m_03 (drawn stamp, read next tick -> conditional
// rand() -> RNG seed) and Sparkle m_Life. The native path never reproduces
// those: the real .s runs headless exactly once per call (behaviour-identical
// to normal dispatch) and the native path only reads pool state and captures
// the emitted prims afterwards, so determinism holds by construction. The
// native path only mirrors the GEOMETRY in float.
//
// Three sub-renderers hooked, one render path each (RP_PARTICLES, RP_GLOW,
// RP_SPARKLE): the game calls them independently - a dragon-rescue cutscene
// renders particles every tick but never calls the glow or sparkle renderer,
// while their pools keep active records - so the sub-tick spine must gate each
// feeder on ITS renderer having drawn, not on the family. func_800573C8 is
// called ~9x/frame (one per occlusion/gamestate pass); glows+sparkles
// once/frame. Each call brackets the polybuf cursor and appends to the native
// scene.
//
// Gated by OPENPETE_PARTICLE_VERIFY (dump/A/B) or a native renderer mode
// (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "reextract.h"  /* sub-tick re-extract entry */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "psx_runtime_internal.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"
#include "openpete_interp.h"       /* openpete_interp_present_now (sparkle phase) */
#include "openpete_interp_objs.h"  /* per-slot prev/curr snapshots (sparkle phase) */
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "renderer/cut_guard.h"     /* cut_held_sets_t: the held cut's tick N-1 set */
#include "openpete_psx.h"           /* openpete_psx_to_host_span */
#include "openpete_subtick_view.h"  /* staging a held cut's pool copy */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro-1/gen/spyro1_symbols.gen.h"
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE - fiber-hook cost split */
#include "openpete_thread_roles.h"

extern void     func_800573C8(CPUState* cpu);  /* RenderParticles  */
extern void     func_800580F4(CPUState* cpu);  /* RenderGlows      */
extern void     func_800584C4(CPUState* cpu);  /* RenderSparkles   */
extern void     func_80058864(CPUState* cpu);  /* RenderDragonStar (free-a-dragon flash) */
extern unsigned openpete_frame;

/* ---- gamestate addresses (psx.map) --------------------------------------- */

/* ---- captured prims (ground truth) --------------------------------------- *
 * Particle FT4 quads (GPU code 0x2C/0x2E) + glow G3 rings (0x32) decode with
 * the standard PSX polygon layout (code = high byte of word[+0x04]; word[+0x00]
 * is the OT tag whose high byte is the *length*, low24 the link). Tiles (0x60),
 * lines (0x40/0x48) and the sparkle quads are NON-polygon prims: kept as raw
 * words: the dump prints them and the A/B compares read the words directly. */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1]>>24)  */
    uint8_t  taglen;        /* OT tag length (word[0]>>24)       */
    int      nv;            /* decoded polygon verts (3/4), else 0 */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad, stp;
    uint32_t raw[16];
    int      nraw;
} prim_t;

#define MAX_PRIMS 8192
static prim_t s_prim[MAX_PRIMS]; static int s_prim_n;
/* The g_Particles pool pointer is double-buffered and swapped between render
 * calls, so reading it AFTER the .s runs gets the wrong (other) buffer. The
 * hook stashes the pointer the .s actually walked at hook-entry. */
static uint32_t s_part_pool;
static uint32_t s_code_hist[256];  /* keyed on the GPU code byte (word[+4]>>24) */
static uint32_t s_tag_hist[256];   /* keyed on the OT tag length (word[+0]>>24) */

/** @brief Appends one walked primitive to s_prim.
 *
 * Copies the decoded fields and the raw words. Tiles and lines are not
 * polygons: they arrive with nv = 0 and only their raw words, which the dump
 * prints and the A/B compares read.
 * @param ctx Unused.
 * @param s The walked primitive.
 * @return 1 when stored, 0 when s_prim holds MAX_PRIMS entries. */
static int particle_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_prim_n >= MAX_PRIMS) return 0;
    prim_t* t = &s_prim[s_prim_n];
    memset(t, 0, sizeof(*t));
    t->code = s->code; t->taglen = s->taglen; t->nv = s->nv;
    for (int k = 0; k < 4; k++) {
        t->x[k] = s->x[k]; t->y[k] = s->y[k]; t->rgb[k] = s->rgb[k];
        t->u[k] = s->u[k]; t->v[k] = s->v[k];
    }
    t->clut = s->clut; t->tpage = s->tpage;
    t->textured = s->textured; t->gouraud = s->gouraud;
    t->quad = s->quad; t->stp = s->stp;
    memcpy(t->raw, s->raw, sizeof(t->raw));
    t->nraw = s->nraw;
    s_prim_n++;
    return 1;
}

/** @brief Walks the guest polybuf range [start, end) and appends each primitive to s_prim.
 *
 * Appends from s_prim_n, which the caller resets, up to MAX_PRIMS, and adds
 * each primitive's GPU code and tag length to the histograms the dump
 * prints. Particle tags are at most 12 words and the dump prints at most 16
 * raw words per primitive.
 * @param cpu The guest CPU state.
 * @param start The first polybuf byte the renderer wrote.
 * @param end One past the last polybuf byte the renderer wrote. */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    OP_PROF_ACCUM_SCOPE("particle.capture");
    const ngte_walk_policy pol = {
        .tag_len_max = 12, .raw_words_max = 16, .guard = MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, particle_prim_sink, NULL);
}

/* ---- one-shot dump (gamestate pools + raw polybuf + decoded prims) -------- */
/** @brief Prints the particle, sparkle and glow pools, the camera and the GTE projection registers to stderr.
 *
 * Verify-mode diagnostic, run once. Lists the first 16 live g_Particles
 * slots of the pool the renderer walked (s_part_pool, stashed at hook
 * entry) with the live total, every g_Sparkles[8] slot, the active
 * D_80078800 glow slots, the camera's first five projection words and
 * position, and GTE OFX, OFY, H, DQA and DQB.
 * @param cpu The guest CPU state. */
static void dump_pools(CPUState* cpu) {
    fprintf(stderr, "  [particles] g_Particles walk (sel/m_Class Life m03 | +4 +8):");
    uint32_t pp = s_part_pool; int npart = 0;   /* pool the .s walked (hook-entry) */
    for (int i = 0; i < 320; i++, pp += 0x20) {
        uint32_t hdr = cpu->read_word(pp);
        int sel = (int)((hdr >> 8) & 0xFF), mclass = (int)(hdr & 0xFF);
        if (sel == 0xFF) break;                        /* end of pool */
        if (sel & 0x80) continue;                      /* inactive slot (skip) */
        int life = (int)((hdr >> 16) & 0xFF), m03 = (int)((hdr >> 24) & 0xFF);
        if (npart < 16) fprintf(stderr, " [%d]s%d/c%d L%d d%d(%08X,%08X)", i, sel, mclass, life, m03,
                                cpu->read_word(pp+4), cpu->read_word(pp+8));
        npart++;
    }
    fprintf(stderr, "  (total live=%d)\n", npart);

    fprintf(stderr, "  [sparkles] g_Sparkles[8] (pos | Life Speed type | color size):\n");
    for (int i = 0; i < 8; i++) {
        uint32_t sp = G_SPARKLES + (uint32_t)i*0x18;
        int x = (int)cpu->read_word(sp+0), y = (int)cpu->read_word(sp+4), z = (int)cpu->read_word(sp+8);
        uint32_t w3 = cpu->read_word(sp+0xC), col = cpu->read_word(sp+0x10), w5 = cpu->read_word(sp+0x14);
        fprintf(stderr, "    [%d] (%d,%d,%d) life=%d spd=%d t=%d col=%06X size=%d\n", i, x, y, z,
                (int)(w3&0xFF), (int)((w3>>8)&0xFF), (int)((w3>>24)&0xFF), col&0xFFFFFF, (int)((w5>>8)&0xFF));
    }

    fprintf(stderr, "  [glows] D_80078800[16] (active | MobyPos* color size | offset bias):\n");
    for (int i = 0; i < 16; i++) {
        uint32_t g = D_GLOWS + (uint32_t)i*0x24;
        uint32_t act = cpu->read_word(g+0);
        if (act == 0) continue;
        uint32_t mp = cpu->read_word(g+0x08), col = cpu->read_word(g+0x0C), sz = cpu->read_word(g+0x10);
        int ox = (int)cpu->read_word(g+0x14), oy = (int)cpu->read_word(g+0x18), oz = (int)cpu->read_word(g+0x1C);
        int bias = (int)cpu->read_word(g+0x20);
        fprintf(stderr, "    [%d] act=%X mobypos=%08X col=%06X size=%d off=(%d,%d,%d) bias=%d\n",
                i, act, mp, col&0xFFFFFF, (int)sz, ox, oy, oz, bias);
    }
    fprintf(stderr, "  camera projMat:");
    for (int i = 0; i < 5; i++) fprintf(stderr, " %08X", cpu->read_word(G_CAMERA+(uint32_t)i*4));
    fprintf(stderr, "  pos=(%d,%d,%d)\n", (int)cpu->read_word(G_CAMERA+0x28),
            (int)cpu->read_word(G_CAMERA+0x2C), (int)cpu->read_word(G_CAMERA+0x30));
    fprintf(stderr, "  OFX=%d OFY=%d H=%d DQA=%d DQB=%d\n",
            (int)cpu->gte_ctrl[24]>>16, (int)cpu->gte_ctrl[25]>>16, (int)(cpu->gte_ctrl[26]&0xFFFF),
            (int)cpu->gte_ctrl[27], (int)cpu->gte_ctrl[28]);
}

/** @brief Prints the captured polybuf range and the decoded primitives to stderr.
 *
 * Verify-mode diagnostic, run once per renderer. Prints the range and size
 * with the s_prim count, up to 40 rows of four raw polybuf words, the code
 * and tag-length histograms, and the first eight s_prim entries with their
 * raw words and, for polygons, the decoded vertices, UVs, CLUT and tpage.
 * @param cpu The guest CPU state.
 * @param tag The renderer label printed in the header and footer.
 * @param start The first polybuf byte the renderer wrote.
 * @param end One past the last polybuf byte the renderer wrote. */
static void dump_prims(CPUState* cpu, const char* tag, uint32_t start, uint32_t end) {
    fprintf(stderr, "\n=== [%s-dump f=%u] polybuf [%08X,%08X) %u bytes, %d prims ===\n",
            tag, openpete_frame, start, end, end - start, s_prim_n);
    /* linear raw dump (4 words/row) - the only reliable view of non-polygon prims */
    fprintf(stderr, "  RAW words:\n");
    int rows = 0;
    for (uint32_t p = start; p < end && rows < 40; p += 16, rows++) {
        fprintf(stderr, "    +%04X:", p - start);
        for (int i = 0; i < 4 && p + (uint32_t)i*4 < end; i++)
            fprintf(stderr, " %08X", cpu->read_word(p + (uint32_t)i*4));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  code histogram (word[+4]>>24):");
    for (int c = 0; c < 256; c++) if (s_code_hist[c]) fprintf(stderr, " %02X:%u", c, s_code_hist[c]);
    fprintf(stderr, "\n  tag-len histogram (word[+0]>>24):");
    for (int c = 0; c < 256; c++) if (s_tag_hist[c]) fprintf(stderr, " %d:%u", c, s_tag_hist[c]);
    fprintf(stderr, "\n");
    int n = s_prim_n < 8 ? s_prim_n : 8;
    for (int i = 0; i < n; i++) {
        prim_t* t = &s_prim[i];
        fprintf(stderr, "  prim[%d] code=%02X taglen=%d nv=%d tex=%d gour=%d quad=%d stp=%d  raw:",
                i, t->code, t->taglen, t->nv, t->textured, t->gouraud, t->quad, t->stp);
        for (int k = 0; k < t->nraw; k++) fprintf(stderr, " %08X", t->raw[k]);
        if (t->nv) {
            fprintf(stderr, "\n      decoded:");
            for (int k = 0; k < t->nv; k++)
                fprintf(stderr, " v%d(%.0f,%.0f rgb=%06X uv=%d,%d)", k, t->x[k], t->y[k], t->rgb[k], t->u[k], t->v[k]);
            if (t->textured) fprintf(stderr, " clut=%04X tpage=%04X", t->clut, t->tpage);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "=== end %s-dump ===\n\n", tag);
}

/* ---- native float walk ------------------------------------------------- *
 * particle_build re-derives the geometry of the g_Particles pool from
 * gamestate in float. particle_feed_native feeds its quads and lines to the
 * native scene; under OPENPETE_PARTICLE_VERIFY, particle_compare checks them
 * against the prims the .s emitted. Pool layout and the emitters covered:
 *   - g_Particles (0x80075824) is a POINTER to the buffer (deref before walking).
 *   - dispatch class = the HIGH byte of the lh header (m_Type/"sel"); the LOW
 *     byte is m_Class (texture/clut index).
 *   - position layout: X=lo16(+0x04), Y=hi16(+0x04), Z=lo16(+0x08), all in
 *     world/4 units; camera-relative VX=X-camX/4, VY=camY/4-Y, VZ=camZ/4-Z.
 *   - sel 0 is a 1-px tile (block_80057F98), sel 1 and 4 are gouraud lines
 *     (block_80057E7C, block_80057954), sel 2 is an affine sin/cos billboard
 *     (block_80057C08), sel 3 a depth-cued screen rect (block_80057A74), sel 5
 *     an affine HUD burst (block_80057750), and sel >= 6 the rotated billboard
 *     (block_800574F8).
 * Glows and sparkles have their own build, feed and compare functions below. */
/* WORLD particles project through proj2 with camera-relative coords shifted >>2
 * (world/4), so their view-Z (cz) is 1/4 the shared full-world scale that
 * scene_viewz + the terrain/actor depths use. Scale cz up by 4 at the build
 * sites (where the class is known) so they sort against terrain instead of
 * always being ~4x too near and drawing THROUGH it. HUD bursts (sel 4/5, direct
 * view-space) and glows (raw-cam, full-world) are already full-scale - not
 * touched. cz is feed-only (culls use the raw view-Z), so scaling it is safe. */
#define PART_VZ_WORLD4 4.0f

/** @brief Reads a signed halfword from guest memory through the word reader.
 *
 * @param cpu The guest CPU state.
 * @param addr The halfword's guest address; bit 1 selects the high half of
 *     the aligned word.
 * @return The sign-extended value. */
static int rd_s16(CPUState* cpu, uint32_t addr) {
    uint32_t w = cpu->read_word(addr & ~3u);
    return (int)(int16_t)((addr & 2u) ? (w >> 16) : (w & 0xFFFF));
}
/** @brief Clamps a value to the signed 16-bit range [-32768, 32767].
 *
 * Models the int16 saturation the GTE applies to a MAC value before the
 * perspective multiply. */
static double clamp16(double v) { return v < -32768.0 ? -32768.0 : (v > 32767.0 ? 32767.0 : v); }

/* ---- sub-tick CHANNEL-PHASE interpolation (pool-wide) ---------------------- *
 * The g_Particles position bracket (reextract.c) lerps only X/Y/Z;
 * every other per-tick-animated channel - billboard SPIN (B byte3) and SIZE
 * (B byte2), sel-3 rect half-extents, sel-1/4 line FAR endpoints (B.hi + word C)
 * - was read raw-curr and stepped at 29.913 Hz while the position glided (same
 * class as the gem-sparkle twinkle phase). When the re-extract
 * pass is active (s_phase_now latched by particle_reextract) the build
 * walks lerp those channels between the slot's prev/curr interp-objs snapshots
 * at FLOAT precision. Line/tile COLOURS (+0x0C/+0x10/+0x14 rgb) are HELD at
 * curr, not lerped - colour ramps can encode palette cycling where a linear rgb
 * sweep is wrong; revisit if a stepped colour is ever visible.
 * Canonical inline feeds (hook time, s_phase_now==0) stay integer/byte-exact
 * for the A/B harnesses. Render-side only: particle RAM is never written here. */
static op_time_t s_phase_now = 0;   /* present clock; 0 = phase interp off
                                     * (latched in particle_reextract) */
/* One sim tick in WALL time - the per-slot alpha denominator cap, same global
 * as TICK_PERIOD_NS in reextract.c (--tick-scale aware: run.c
 * rescales it, else slow-motion sweeps the phase in tick_scale of a tick and
 * freezes the rest). */
#define PART_TICK_PERIOD_NS g_openpete_tick_period_ns
/* Phase-alpha floor - the SAME slightly-negative allowance the spine's
 * position/rotation alpha uses (reextract.c: alpha >= -0.35).
 * The canonical present + first intermediates of a tick fire a few ms BEFORE
 * that tick's PLL grid stamp, so their raw alpha is slightly negative;
 * clamping the phase at 0 while positions extrapolate backward freezes the
 * twinkle for those presents and delivers the clipped tail as a jump at
 * every roll - a 29.913 Hz hitch riding an otherwise smooth sweep, sized by
 * the (machine-dependent) present-clock-vs-stamp skew. Backward
 * extrapolation on the pair's slope is continuous across the roll by
 * construction. Never above 1 (the future is unsimulated). */
#define PART_ALPHA_MIN (-0.35)

/** @brief Reports whether OPENPETE_STREAK_DBG enables the long-streak log.
 *
 * When set, streak_log prints each emitted particle line, glow fan leg or
 * sparkle line longer than 300 px on screen with its family and pool slot,
 * at most five lines per openpete_frame value. The variable is read once
 * and cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int streak_dbg(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_STREAK_DBG"); v = (e && *e && *e != '0'); }
    return v;
}
/** @brief Logs a screen-space segment longer than 300 px to stderr.
 *
 * Prints the family, pool slot address and endpoints, at most five lines
 * per openpete_frame value. Callers gate it on streak_dbg.
 * @param fam The emitter family label.
 * @param slot The pool slot's guest address.
 * @param ax The first endpoint's screen X.
 * @param ay The first endpoint's screen Y.
 * @param bx The second endpoint's screen X.
 * @param by The second endpoint's screen Y. */
static void streak_log(const char* fam, uint32_t slot,
                       double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    if (dx*dx + dy*dy < 300.0*300.0) return;
    static unsigned s_n, s_lastf;
    if (openpete_frame != s_lastf) { s_lastf = openpete_frame; s_n = 0; }
    if (s_n++ > 4) return;                     /* max 5 lines per VBL */
    fprintf(stderr, "[streak f=%u] %s slot=0x%X (%.0f,%.0f)->(%.0f,%.0f)\n",
            openpete_frame, fam, slot, ax, ay, bx, by);
}

/** @brief Computes an interp-objs slot's sub-tick phase alpha at the present clock.
 *
 * The snapshot interval is capped at one tick period; the alpha is
 * (s_phase_now - curr_ns) / interval clamped to [PART_ALPHA_MIN, 1].
 * @param o The slot's interp-objs entry.
 * @param a Receives the alpha; unwritten on failure.
 * @return 1 when the interval is positive, else 0. */
static int part_slot_alpha(const openpete_interp_obj_t* o, double* a) {
    op_time_t per = o->curr_ns - o->prev_ns;
    if (per > PART_TICK_PERIOD_NS) per = PART_TICK_PERIOD_NS;
    if (per <= 0) return 0;
    double v = (double)(s_phase_now - o->curr_ns) / (double)per;
    if (v < PART_ALPHA_MIN) v = PART_ALPHA_MIN; else if (v > 1.0) v = 1.0;
    *a = v;
    return 1;
}

/* The prev and curr interp-obj words A/B/C (+0x04, +0x08, +0x0C) of a
 * g_Particles slot and the slot's sub-tick alpha. */
typedef struct { uint32_t A, B, C;      /* table-PREV words */
                 uint32_t cA, cB, cC;   /* table-CURR words - the forward
                                          * endpoint. NEVER live RAM: under
                                          * runahead live = committed tick ≈
                                          * table-prev, degenerating every
                                          * channel lerp to the tick rate. */
                 double alpha; } part_prev_t;
/** @brief Looks up a g_Particles slot's previous-tick words for channel interpolation.
 *
 * Passes only when the same identity guards the position bracket applies
 * (reextract.c pool loop) hold: a slot whose position snapped (spawn, reuse,
 * teleport) must not lerp its other channels, or spin and size would sweep
 * across two different particles. Fails when the re-extract phase clock
 * s_phase_now is 0, the slot has no previous snapshot, m_Class or sel
 * changed, the life byte dropped, the base position moved more than 300
 * world/4 units, or the snapshot interval is not positive. The interval is
 * capped at one tick period, and alpha is (s_phase_now - curr_ns) /
 * interval clamped to [PART_ALPHA_MIN, 1].
 * @param pp The slot's guest address.
 * @param out Receives the prev words A, B, C, the curr words cA, cB, cC and
 *     the alpha; unwritten on failure.
 * @return 1 when the channels may be interpolated, else 0. */
static int part_phase_prev(uint32_t pp, part_prev_t* out) {
    if (!s_phase_now) return 0;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(pp);
    if (!o || !o->have_prev) return 0;                 /* spawn -> snap */
    uint32_t ph, ch;
    memcpy(&ph, o->prev + 0x00, 4);
    memcpy(&ch, o->curr + 0x00, 4);
    if ((ph & 0xFFFFu) != (ch & 0xFFFFu)) return 0;    /* m_Class/sel changed -> reuse */
    uint8_t plife = (uint8_t)(ph >> 16), clife = (uint8_t)(ch >> 16);
    if (clife < plife) return 0;                       /* life reset -> reuse */
    uint32_t pA, pB, pC, cA, cB, cC;
    memcpy(&pA, o->prev + 0x04, 4); memcpy(&pB, o->prev + 0x08, 4);
    memcpy(&pC, o->prev + 0x0C, 4);
    memcpy(&cA, o->curr + 0x04, 4); memcpy(&cB, o->curr + 0x08, 4);
    memcpy(&cC, o->curr + 0x0C, 4);
    double dX = (double)(int16_t)(cA & 0xFFFF) - (double)(int16_t)(pA & 0xFFFF);
    double dY = (double)(int16_t)(cA >> 16)    - (double)(int16_t)(pA >> 16);
    double dZ = (double)(int16_t)(cB & 0xFFFF) - (double)(int16_t)(pB & 0xFFFF);
    double d2 = dX*dX + dY*dY + dZ*dZ;
    /* Teleport/reuse ceiling, particle-physics scale (was 1250 == 5000
     * full-world, with a tighter 300 only for the stalled-life class):
     * under runahead the pair can span two prediction BRANCHES (the led
     * timeline re-forks whenever the held input differs from what the
     * committed tick latches), and pool churn differs between branches -
     * slot reuse across branches passes the class + life-increase tests
     * (no same-timeline ordering to reset the life byte), and anything
     * under the old ceiling lerped as a screen-crossing streak. 300
     * world/4 per tick == 1200 full-world == ~20x Spyro's run speed: a
     * REAL particle faster than that crosses the screen in a couple of
     * ticks, so snapping it loses nothing visible - beyond this, lerping
     * only ever paints streaks. */
    if (d2 > 300.0*300.0) return 0;
    double a;
    if (!part_slot_alpha(o, &a)) return 0;
    out->A = pA; out->B = pB; out->C = pC;
    out->cA = cA; out->cB = cB; out->cC = cC;
    out->alpha = a;
    return 1;
}

/** @brief Interpolates a byte channel from p to c at alpha a, clamped to [0, 255].
 *
 * A negative alpha extrapolates backward past p; the clamp keeps a size or
 * colour byte channel in range.
 * @param p The previous tick's byte.
 * @param c The current tick's byte.
 * @param a The sub-tick alpha, at least PART_ALPHA_MIN.
 * @return The interpolated value. */
static double lerp_b(int p, int c, double a) {
    double v = (double)p + ((double)c - (double)p) * a;
    return v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v);
}
/** @brief Interpolates a byte angle (256 per circle) from p to c along the shorter arc.
 *
 * The result is not wrapped back into [0, 256).
 * @param p The previous tick's angle byte.
 * @param c The current tick's angle byte.
 * @param a The sub-tick alpha.
 * @return The interpolated angle. */
static double lerp_wrap_b(int p, int c, double a) {
    int d = (c - p) & 0xFF; if (d >= 128) d -= 256;
    return (double)p + (double)d * a;
}

/** @brief Reports whether OPENPETE_PART_PHASE_DBG enables the particle phase trace.
 *
 * When set, each particle build walk prints one line for the first
 * phase-interpolated particle with its interpolated size and spin channels
 * and the raw bytes. The variable is read once and cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int part_phase_dbg(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_PART_PHASE_DBG"); s = (e && *e && *e != '0'); }
    return s;
}
static int s_ppdbg_done = 0;   /* reset per build walk */
/** @brief Reports whether OPENPETE_GLOW_ANIM_DBG enables the glow animation trace.
 *
 * The trace is a census of raw glow-record changes plus a per-present
 * phase line. The variable is read once and cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int glow_anim_dbg(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_GLOW_ANIM_DBG"); s = (e && *e && *e != '0'); }
    return s;
}

/* The projection view of the running build (camera, offsets, H, Hor+
 * bounds); each build entry rebuilds it, the cull helpers read it. */
static ngte_view s_pv;

/** @brief Reports whether an inline projection falls in the GTE divide-cap regime.
 *
 * The cap applies when SZ3 <= 0 or SZ3 <= H/2 at the GTE's H (Hc). The
 * verdict at the display H (He) is only counted in the NGTE_SITE_PART_CAP
 * census.
 * @param sz3 The clamped SZ3.
 * @param Hc The canonical projection distance.
 * @param He The display projection distance.
 * @return 1 when the quotient is capped, else 0. */
static int part_cap(double sz3, double Hc, double He) {
    int c = (sz3 <= 0.0 || Hc >= 2.0 * sz3);
    NGTE_LIMIT(NGTE_SITE_PART_CAP, c, sz3 <= 0.0 || He >= 2.0 * sz3);
    return c;
}

/* RTPS one view-space vertex (TR=0) through R -> screen + the view-space MAC */
typedef struct { double sx, sy, m1, m2, m3; } projres;

static void proj_view(double ofx, double ofy, const ngte_view* v,
                      double X, double Y, double Z, projres* o);
/** @brief Projects a camera-relative vertex through rotation R, as RTPS with TR = 0 does.
 *
 * Runs ngte_project_gte without the +-1024 SXY saturation, so the widened
 * view sees the true X.
 * @param R The camera rotation, Q12.
 * @param ofx The screen X offset in pixels.
 * @param ofy The screen Y offset in pixels.
 * @param v The view.
 * @param vx The vertex's GTE-space X.
 * @param vy The vertex's GTE-space Y.
 * @param vz The vertex's GTE-space Z.
 * @param o Receives the screen position and MAC1..MAC3. */
static void proj2(const int R[3][3], double ofx, double ofy, const ngte_view* v,
                  double vx, double vy, double vz, projres* o) {
    ngte_proj r;   /* shared pipeline, NO ±1024 SXY saturation - float,
                    * widescreen reveal needs the true X */
    ngte_project_gte(R, NULL, vx, vy, vz, ofx, ofy, v, 0, &r);
    o->sx = r.sx; o->sy = r.sy; o->m1 = r.m1; o->m2 = r.m2; o->m3 = r.m3;
}

/** @brief Projects a view-space point directly, without the camera rotation.
 *
 * Sel 4 and 5 particles (HUD bursts) store view-space coordinates that the
 * game loads as the GTE translation with vertex (0, 0, 0), so MAC = (X, Y,
 * Z) and SX = OFX + X * H / Z. Runs ngte_screen_gte without the SXY
 * saturation.
 * @param ofx The screen X offset in pixels.
 * @param ofy The screen Y offset in pixels.
 * @param v The view.
 * @param X The view-space X.
 * @param Y The view-space Y.
 * @param Z The view-space Z.
 * @param o Receives the screen position and the MACs. */
static void proj_view(double ofx, double ofy, const ngte_view* v,
                      double X, double Y, double Z, projres* o) {
    ngte_proj r;   /* view-space point: MAC=(X,Y,Z), divide-only */
    ngte_screen_gte(X, Y, Z, ofx, ofy, v, 0, &r);
    o->sx = r.sx; o->sy = r.sy; o->m1 = r.m1; o->m2 = r.m2; o->m3 = r.m3;
}

/** @brief Applies the .s screen-rect reject to a projected particle centre.
 *
 * Rounds the centre and tests the packed int32 (SY << 16) | (SX & 0xFFFF)
 * as the .s does: packed <= 0x10000 (s4) rejects at the top, packed >=
 * 0x1000000 (s5) at the bottom. The left and right edges (the .s SX <= 0
 * and the s6 0x2000000 test) are the running view's horizontal bounds
 * s_pv.xlo and s_pv.xhi, which equal [0, 512] at 4:3 and widen for a wider
 * view.
 * @param sxf The centre's screen X.
 * @param syf The centre's screen Y.
 * @return 1 to cull, 0 to keep. */
static int screen_rect_cull(double sxf, double syf) {
    int SX = (int)lrint(sxf), SY = (int)lrint(syf);
    int32_t v0  = (int32_t)(((uint32_t)(SY & 0xFFFF) << 16) | (uint32_t)(SX & 0xFFFF));
    if ((v0 - 0x10000) <= 0)    return 1;   /* above top / SY<=1   */
    if ((v0 - 0x1000000) >= 0)  return 1;   /* below bottom SY>=256 */
    /* horizontal edges widen for widescreen (Hor+): [0,512] at 4:3 (exactly the
     * .s SX<=0 / SX>=512 reject), wider when the window is. signed SX so the
     * negative left-reveal isn't mistaken for a wrapped large value. */
    float xlo = s_pv.xlo, xhi = s_pv.xhi;
    if (SX <= (int)xlo)         return 1;    /* left of the screen  */
    if (SX >= (int)xhi)         return 1;    /* right of the screen */
    return 0;
}

/** @brief Applies the default particle class's four-corner screen reject (block_8005768C..80057718).
 *
 * Culls only when all four corners lie outside the same screen edge,
 * tested on the packed int32 (SY << 16) | (SX & 0xFFFF) as the .s does:
 * top against 0x10000 (s4), bottom against 0x1000000 (s5). The left and
 * right edges (the .s SX <= 0 and s6) are the running view's horizontal
 * bounds, which widen past [0, 512] for a wider-than-4:3 view.
 * @param sx The four corners' screen X.
 * @param sy The four corners' screen Y.
 * @return 1 to cull, 0 to keep. */
static int clip4_cull(const float* sx, const float* sy) {
    /* horizontal edges widen for widescreen (Hor+); vertical stays 4:3. */
    float xlo = s_pv.xlo, xhi = s_pv.xhi;
    int top = 1, bot = 1, left = 1, right = 1;
    for (int k = 0; k < 4; k++) {
        int SX = (int)lrintf(sx[k]), SY = (int)lrintf(sy[k]);
        int32_t v   = (int32_t)(((uint32_t)(SY & 0xFFFF) << 16) | (uint32_t)(SX & 0xFFFF));
        if ((v - 0x10000) > 0)     top = 0;
        if ((v - 0x1000000) < 0)   bot = 0;
        if (SX > (int)xlo)         left = 0;
        if (SX < (int)xhi)         right = 0;
    }
    return top | bot | left | right;
}

typedef struct {
    float sx[4], sy[4], sz[4];   /* corner screen XY + per-corner view-Z */
    float cz;                     /* representative centre view-Z (depth sort) */
    int sel;
    uint32_t rgb;                 /* modulation colour (particle +0x0C low24) */
    int u[4], v[4];               /* per-corner texel coords */
    int tpx, tpy, clx, cly, bpp;  /* VRAM material */
    int textured, blend;          /* 0 opaque / 1 alpha / 2 add / 3 sub */
} fquad;
#define MAX_FQUAD 2048
static fquad s_fq[MAX_FQUAD]; static int s_fq_n;

/* sel-1 gouraud motion-streak LINE_G (code 0x50): a line between two
 * camera-relative world positions with a per-endpoint colour. */
typedef struct {
    float ax, ay, bx, by;        /* projected endpoints */
    uint32_t rgb0, rgb1;         /* per-endpoint colour (gouraud) */
    float cz;                    /* EP0 view-Z (depth) */
    int blend;
    int sel;                     /* 1 = world streak; 4 = HUD egg-burst (view-space) */
} pline;
#define MAX_PLINE 1024
static pline s_pline[MAX_PLINE]; static int s_pline_n;

/** @brief Fills a quad's material as flat colour from a particle's +0x0C command word.
 *
 * The low 24 bits are the colour; STP (bit 1 of the GPU code byte) selects
 * blend 1, else opaque. UVs and the VRAM material are zero.
 * @param cmd The record's +0x0C word.
 * @param q Receives the colour, blend and a zero texture material. */
static void material_flat(uint32_t cmd, fquad* q) {
    q->rgb = cmd & 0xFFFFFF;
    q->textured = 0;
    q->blend = ((cmd >> 24) & 0x02) ? 1 : 0;
    for (int k = 0; k < 4; k++) { q->u[k] = q->v[k] = 0; }
    q->tpx = q->tpy = q->clx = q->cly = q->bpp = 0;
}

/** @brief Decodes a particle's FT4 material from gamestate into a native quad.
 *
 * One idiom serves every FT4 class: the colour/command word at +0x0C (high
 * byte the GPU code, bit 1 of it STP), the texture record at
 * D_80076278[m_Class] + (byte +0x10) * 8, UV0 and the CLUT at texrec + 4,
 * UV3 and the tpage at texrec + 8, and the other two corners the (u3, v0)
 * and (u0, v3) split, corner k taking UVk in the .s vertex order. A class
 * with no texture table entry decodes as flat. The blend is the tpage's ABR
 * mode when STP is set (the quarter rate drawn as additive), else opaque.
 * @param cpu The guest CPU state.
 * @param pp The particle record's guest address.
 * @param mclass The particle's m_Class.
 * @param q Receives the colour, UVs, VRAM material and blend. */
static void decode_material(CPUState* cpu, uint32_t pp, int mclass, fquad* q) {
    uint32_t cmd = cpu->read_word(pp + 0x0C);
    q->rgb = cmd & 0xFFFFFF;
    int stp = ((cmd >> 24) & 0x02) != 0;          /* GPU code STP bit */
    uint32_t texbase = cpu->read_word(D_PARTTEX + (uint32_t)mclass * 4);
    if (!texbase) { material_flat(cmd, q); return; }   /* no texture record -> flat */
    uint32_t texrec = texbase + (cpu->read_word(pp + 0x10) & 0xFF) * 8u;
    uint32_t w4 = cpu->read_word(texrec + 4), w8 = cpu->read_word(texrec + 8);
    int u0 = (int)(w4 & 0xFF), v0 = (int)((w4 >> 8) & 0xFF), clut = (int)((w4 >> 16) & 0xFFFF);
    int u3 = (int)(w8 & 0xFF), v3 = (int)((w8 >> 8) & 0xFF), tpage = (int)((w8 >> 16) & 0xFFFF);
    q->u[0]=u0; q->v[0]=v0; q->u[1]=u3; q->v[1]=v0; q->u[2]=u0; q->v[2]=v3; q->u[3]=u3; q->v[3]=v3;
    ngte_tpage tp;
    ngte_decode_tpage((unsigned)tpage, (unsigned)clut, 0u, &tp);
    q->tpx = tp.tpx; q->tpy = tp.tpy;
    q->bpp = tp.bpp;
    q->clx = tp.clx; q->cly = tp.cly;
    q->textured = 1;
    /* data-driven blend (ABR + STP); rate 3 has no native pipeline */
    q->blend = stp ? ngte_abr_blend(tp.abr, NGTE_ABR_QUARTER_AS_ADD) : 0;
}

/** @brief Applies the .s SZ window and depth-cue LOD cull to a particle.
 *
 * Clamps MAC3 to [0, 65535] as SZ3, rejects SZ3 outside [128, 8192), and
 * rejects when (SZ3 >> 5) < texsub_hi.
 * @param mac3 The centre's view-space MAC3.
 * @param texsub_hi The high byte of the record's low halfword at +0x10.
 * @return 1 to cull, 0 to keep. */
static int sz_and_lod_cull(double mac3, int texsub_hi) {
    int sz3 = (int)(mac3 < 0 ? 0 : (mac3 > 65535 ? 65535 : mac3));
    if (sz3 < 128 || sz3 >= 8192) return 1;
    if (((sz3 >> 5) - texsub_hi) < 0) return 1;
    return 0;
}

/** @brief Returns the paint depth of a sel-4 egg-tally line: its ordering-table bucket in SZ3 units.
 *
 * The .s links the line into bucket max(0, (SZ3 >> 5) - lod), lod the high
 * byte of the record's +0x14 word (80057A28-80057A40). The depth is
 * (bucket + 1) << 5, so the flat HUD layer, which paints back to front by
 * depth and keeps submission order on a tie, draws the lines in the game's
 * bucket order.
 * @param sz3 EP0's SZ3.
 * @param col1 The record's +0x14 word.
 * @return The depth, in SZ3 units. */
static float sel4_bucket_depth(int sz3, uint32_t col1) {
    const int bucket = (sz3 >> 5) - (int)(col1 >> 24);
    return (float)(((bucket > 0 ? bucket : 0) + 1) << 5);
}

/* The running particle build's view inputs, shared by every sel builder. */
typedef struct {
    CPUState* cpu;
    const ngte_view* v;          /* s_pv */
    ngte_view vc;                /* canonical view: the HUD bursts (sel 4, 5) */
    int R[3][3];                 /* camera rotation */
    double s7, t8, t9;           /* camera position / 4, fractional */
    double ofx, ofy;             /* screen offset */
    double H, Hc, qcap;          /* display H, the GTE's H, capped quotient */
} part_ctx_t;

/* One live g_Particles record as the build reads it. */
typedef struct {
    uint32_t pp;                 /* the record's guest address */
    uint32_t A, B;               /* words +0x04, +0x08 */
    int slot, sel, mclass;       /* pool index, dispatch byte, texture/clut index */
    int texsub_hi;               /* high byte of the low halfword at +0x10 */
    double VXb, VYb, VZb;        /* camera-relative centre: GTE Z, X, Y */
} part_rec_t;

/** @brief Clamps a view-space MAC3 to the GTE's SZ3 range [0, 65535].
 *
 * @param m3 The MAC3.
 * @return The clamped value. */
static double sz3_sat(double m3) { return m3 < 0 ? 0 : (m3 > 65535 ? 65535 : m3); }

/** @brief Projects a particle record's centre through the camera, as the .s centre RTPS does.
 *
 * @param x The build's view inputs.
 * @param r The record.
 * @param c Receives the screen position and MACs. */
static void part_centre(const part_ctx_t* x, const part_rec_t* r, projres* c) {
    proj2(x->R, x->ofx, x->ofy, x->v, r->VYb, r->VZb, r->VXb, c);
}

/** @brief Projects GTE-space offsets about a translation through a diagonal scale and the capped quotient.
 *
 * Point k is MAC1 = TR0 + T[k][0] * kx, MAC2 = TR1 + T[k][1] * ky, each
 * saturated to int16 and multiplied by qd, then offset by (ofx, ofy): the
 * GPF projection the particle billboards (diag(4096, 2560, 4096), kx = 1,
 * ky = 0.625) and the sparkle tips (diag(a3), kx = ky = a3 / 4096) share.
 * @param T The offsets.
 * @param n The point count.
 * @param TR0 The translation's MAC1.
 * @param TR1 The translation's MAC2.
 * @param kx The X scale.
 * @param ky The Y scale.
 * @param qd The projection quotient.
 * @param ofx The screen X offset.
 * @param ofy The screen Y offset.
 * @param sx Receives n screen X values.
 * @param sy Receives n screen Y values. */
static void affine_project(const double (*T)[2], int n, double TR0, double TR1,
                           double kx, double ky, double qd, double ofx, double ofy,
                           double* sx, double* sy) {
    for (int k = 0; k < n; k++) {
        sx[k] = ofx + clamp16(TR0 + T[k][0] * kx) * qd;
        sy[k] = ofy + clamp16(TR1 + T[k][1] * ky) * qd;
    }
}

/** @brief Returns a spinning particle's corner extents from its size byte and angle byte.
 *
 * The size is B byte 2 and the angle B byte 3, offset by ang_ofs sine-table
 * entries (32 for the sel 2 and 5 billboards, 0 for sel >= 6). Integer path:
 * the sine and cosine table entries scaled by size / div in float. While the
 * slot's phase interpolates, the size is lerped and the angle wrap-lerped
 * between the slot's snapshots and the sine and cosine taken in float,
 * which equals the integer path at alpha 1.
 * @param x The build's view inputs.
 * @param r The record.
 * @param ang_ofs The angle offset in table entries.
 * @param div The extent divisor (1024 or 4096).
 * @param dbg Nonzero to print the OPENPETE_PART_PHASE_DBG line.
 * @param es Receives size * sin / div.
 * @param ec Receives size * cos / div. */
static void part_spin_extents(const part_ctx_t* x, const part_rec_t* r, int ang_ofs, double div,
                              int dbg, double* es, double* ec) {
    const uint32_t B = r->B;
    int sizeB = (int)((B >> 16) & 0xFF);
    int angIdx = (int)(((B >> 23) + (uint32_t)(2 * ang_ofs)) & 0x1FE);
    part_prev_t pv;                              /* sub-tick spin/size sweep */
    if (part_phase_prev(r->pp, &pv)) {
        /* angle channel = B byte3 (angIdx == byte3 + ang_ofs entries - bit 23
         * is cleared by the &0x1FE); wrap-lerp it, then float sin/cos. */
        double szf = lerp_b((int)((pv.B >> 16) & 0xFF), (int)((pv.cB >> 16) & 0xFF), pv.alpha);
        double ef  = lerp_wrap_b((int)(pv.B >> 24), (int)(pv.cB >> 24), pv.alpha) + (double)ang_ofs;
        double sf, cf; ngte_sincos_f(x->cpu, ef, &sf, &cf);
        *es = szf * sf / div; *ec = szf * cf / div;
        if (dbg && part_phase_dbg() && !s_ppdbg_done) { s_ppdbg_done = 1;
            fprintf(stderr, "[part-phase f=%u] slot=%d sel=%d lerp ang=%.2f sz=%.2f (raw %d/%d)\n",
                    openpete_frame, r->slot, r->sel, ef, szf, (int)(B >> 24), sizeB); }
        return;
    }
    int sinv = rd_s16(x->cpu, D_SIN_TABLE + (uint32_t)angIdx);
    int cosv = rd_s16(x->cpu, D_SIN_TABLE + (uint32_t)angIdx + 0x80);
    *es = (double)sizeB * sinv / div; *ec = (double)sizeB * cosv / div;  /* float, not a shift: sub-pixel extent */
}

/** @brief Fills a quad's corners for the affine sin/cos billboard of sel 2 and sel 5.
 *
 * The corners (-a2,-a3), (a3,-a2), (-a3,a2), (a2,a3) go through
 * diag(4096, 2560, 4096) plus the translation and the capped quotient.
 * @param x The build's view inputs.
 * @param a2 The sine extent.
 * @param a3 The cosine extent.
 * @param TR0 The translation's MAC1.
 * @param TR1 The translation's MAC2.
 * @param TR2 The translation's MAC3, stored as each corner's depth.
 * @param qd The projection quotient.
 * @param q Receives the corner positions and depths. */
static void part_affine_quad(const part_ctx_t* x, double a2, double a3,
                             double TR0, double TR1, double TR2, double qd, fquad* q) {
    const double C[4][2] = { {-a2,-a3}, {a3,-a2}, {-a3,a2}, {a2,a3} };
    double sx[4], sy[4];
    affine_project(C, 4, TR0, TR1, 1.0, 2560.0 / 4096.0, qd, x->ofx, x->ofy, sx, sy);
    for (int k = 0; k < 4; k++) { q->sx[k] = (float)sx[k]; q->sy[k] = (float)sy[k]; q->sz[k] = (float)TR2; }
}

/** @brief Interpolates a line particle's far endpoint between the slot's snapshots.
 *
 * The slot's position guard covers only the base word, so the far endpoint
 * has its own ceiling: a move over 300 units per tick (a cross-branch pair,
 * or an unsigned decode straddling a wrap) snaps to the current snapshot.
 * @param p The previous snapshot's endpoint.
 * @param c The current snapshot's endpoint.
 * @param a The sub-tick alpha.
 * @param out Receives the endpoint. */
static void far_endpoint_lerp(const double p[3], const double c[3], double a, double out[3]) {
    double dx = c[0] - p[0], dy = c[1] - p[1], dz = c[2] - p[2];
    if (dx*dx + dy*dy + dz*dz <= 300.0*300.0) {
        out[0] = p[0] + dx * a; out[1] = p[1] + dy * a; out[2] = p[2] + dz * a;
    } else {
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
    }
}

/** @brief Appends a gouraud line between two projected endpoints to s_pline.
 *
 * @param pp The record's guest address, for the streak log.
 * @param e0 The first endpoint's projection.
 * @param e1 The second endpoint's projection.
 * @param col0 The record's +0x10 word: EP0 colour, GPU code in the high byte.
 * @param col1 The record's +0x14 word: EP1 colour.
 * @param cz The line's depth.
 * @param sel The record's sel (1 or 4).
 * @param fam The streak-log family label. */
static void part_push_line(uint32_t pp, const projres* e0, const projres* e1,
                           uint32_t col0, uint32_t col1, float cz, int sel, const char* fam) {
    pline* l = &s_pline[s_pline_n++];
    l->ax=(float)e0->sx; l->ay=(float)e0->sy; l->bx=(float)e1->sx; l->by=(float)e1->sy;
    l->rgb0 = col0 & 0xFFFFFF; l->rgb1 = col1 & 0xFFFFFF;
    l->cz = cz;
    l->blend = ((col0 >> 24) & 0x02) ? 1 : 0;     /* code 0x50 -> opaque */
    l->sel = sel;
    if (streak_dbg()) streak_log(fam, pp, e0->sx, e0->sy, e1->sx, e1->sy);
}

/** @brief Builds a sel-0 particle: the 2x1-pixel dot the .s emits as a LINE_F (block_80057F98).
 *
 * Culls on the SZ window (SZ3 nonzero, below 8192), the centre's screen
 * rect and the depth-cue LOD (s3 = 2); the .s draws (SX, SY) to (SX + 1,
 * SY), whose two plotted endpoints cover 2x1 pixels.
 * @param x The build's view inputs.
 * @param r The record. */
static void part_build_tile(const part_ctx_t* x, const part_rec_t* r) {
    projres c; part_centre(x, r, &c);
    int sz3 = (int)sz3_sat(c.m3);
    if (sz3 == 0 || sz3 >= 8192) return;        /* SZ window (SZ3!=0 .. 8192) */
    if (screen_rect_cull(c.sx, c.sy)) return;
    int lod = (int)((r->B >> 16) & 0xFF);
    if (((sz3 >> 5) - lod - 2) <= 0) return;    /* depth-cue LOD cull (s3=2) */
    fquad q; q.sel = r->sel;
    ngte_line_quad((float)c.sx, (float)c.sy, (float)(c.sx + 1.0), (float)c.sy,
                   0.0f, q.sx, q.sy);
    for (int k = 0; k < 4; k++) q.sz[k] = (float)c.m3;
    q.cz = (float)c.m3 * PART_VZ_WORLD4;         /* world/4 -> full-world */
    material_flat(x->cpu->read_word(r->pp + 0x0C), &q);   /* flat colour, no texture */
    s_fq[s_fq_n++] = q;
}

/** @brief Builds a sel-1 gouraud motion streak, a LINE_G between two camera-relative world points (block_80057E7C).
 *
 * EP0 is the record's centre; EP1 is (B >> 16, C & 0xFFFF, C >> 16) with C
 * the +0x0C word, decoded unsigned, under the same camera-relative /4
 * swizzle. Culls on EP0's SZ window, screen rect and depth-cue LOD (s3 =
 * 2, lod the high byte of +0x14). While the slot's phase interpolates, EP1
 * is far_endpoint_lerp'ed; EP0 rides the position bracket.
 * @param x The build's view inputs.
 * @param r The record. */
static void part_build_streak(const part_ctx_t* x, const part_rec_t* r) {
    if (s_pline_n >= MAX_PLINE) return;
    uint32_t C = x->cpu->read_word(r->pp + 0x0C);
    double e1w[3] = { (double)(r->B >> 16), (double)(C & 0xFFFF), (double)(C >> 16) };
    part_prev_t pv;
    if (part_phase_prev(r->pp, &pv)) {
        const double p[3] = { (double)(pv.B >> 16),  (double)(pv.C & 0xFFFF),  (double)(pv.C >> 16) };
        const double c[3] = { (double)(pv.cB >> 16), (double)(pv.cC & 0xFFFF), (double)(pv.cC >> 16) };
        far_endpoint_lerp(p, c, pv.alpha, e1w);
    }
    projres e0, e1;
    part_centre(x, r, &e0);                                                          /* EP0 */
    proj2(x->R, x->ofx, x->ofy, x->v, (x->t8 - e1w[1]), (x->t9 - e1w[2]), (e1w[0] - x->s7), &e1);   /* EP1 */
    int sz3 = (int)sz3_sat(e0.m3);
    if (sz3 == 0 || sz3 >= 8192) return;         /* EP0 SZ window */
    if (screen_rect_cull(e0.sx, e0.sy)) return;  /* EP0 screen-rect */
    uint32_t col0 = x->cpu->read_word(r->pp + 0x10), col1 = x->cpu->read_word(r->pp + 0x14);
    int lod = (int)(col1 >> 24);
    if (((sz3 >> 5) - lod - 2) <= 0) return;     /* EP0 depth-cue LOD (s3=2) */
    part_push_line(r->pp, &e0, &e1, col0, col1, (float)e0.m3 * PART_VZ_WORLD4, 1, "line1");
}

/** @brief Builds a sel-2 affine sin/cos billboard FT4.
 *
 * Culls on the centre's screen rect and the SZ window and LOD; the corners
 * are the spin extents (ang_ofs 32, div 1024) about TR = centre MAC << 2,
 * projected at the capped quotient. Depth is the centre's view-Z.
 * @param x The build's view inputs.
 * @param r The record. */
static void part_build_affine(const part_ctx_t* x, const part_rec_t* r) {
    projres c; part_centre(x, r, &c);
    if (screen_rect_cull(c.sx, c.sy)) return;
    if (sz_and_lod_cull(c.m3, r->texsub_hi)) return;
    double a2, a3;
    part_spin_extents(x, r, 32, 1024.0, 1, &a2, &a3);
    double TR0 = c.m1*4.0, TR1 = c.m2*4.0, TR2 = c.m3*4.0;
    double sz3 = sz3_sat(TR2);
    double qd = part_cap(sz3, x->Hc, x->H) ? x->qcap : x->H/sz3;
    fquad q; q.sel = r->sel;
    part_affine_quad(x, a2, a3, TR0, TR1, TR2, qd, &q);
    q.cz = (float)c.m3 * PART_VZ_WORLD4;          /* world/4 -> full-world (centre depth, not TR2) */
    decode_material(x->cpu, r->pp, r->mclass, &q);
    s_fq[s_fq_n++] = q;
}

/** @brief Builds a sel-3 depth-cued screen rect FT4.
 *
 * Culls on the centre's screen rect and the SZ window and LOD. The half
 * extents are the size (B byte 2) and alpha (B byte 3) bytes times the GTE
 * reciprocal H / SZ3 capped at 1.0, kept in float where the .s truncates
 * IR0 and the extents to whole pixels; the bytes are lerped while the slot's
 * phase interpolates.
 * @param x The build's view inputs.
 * @param r The record. */
static void part_build_rect(const part_ctx_t* x, const part_rec_t* r) {
    projres c; part_centre(x, r, &c);
    if (screen_rect_cull(c.sx, c.sy)) return;
    if (sz_and_lod_cull(c.m3, r->texsub_hi)) return;
    double sz3 = sz3_sat(c.m3);
    double qd = sz3 <= 0.0 ? 0.0 : x->H / sz3;
    double ir0 = qd < 0.0 ? 0.0 : (qd > 1.0 ? 1.0 : qd);   /* GTE caps the reciprocal at 1.0 */
    int sizeB = (int)((r->B >> 16) & 0xFF), alphaB = (int)((r->B >> 24) & 0xFF);
    double szf = (double)sizeB, alf = (double)alphaB;
    part_prev_t pv;                              /* sub-tick half-extent sweep */
    if (part_phase_prev(r->pp, &pv)) {
        szf = lerp_b((int)((pv.B >> 16) & 0xFF), (int)((pv.cB >> 16) & 0xFF), pv.alpha);
        alf = lerp_b((int)((pv.B >> 24) & 0xFF), (int)((pv.cB >> 24) & 0xFF), pv.alpha);
        if (part_phase_dbg() && !s_ppdbg_done) { s_ppdbg_done = 1;
            fprintf(stderr, "[part-phase f=%u] slot=%d sel=3 lerp sz=%.2f al=%.2f (raw %d/%d)\n",
                    openpete_frame, r->slot, szf, alf, sizeB, alphaB); }
    }
    double a2 = szf * ir0, a3 = alf * ir0;       /* halfW, halfH (sub-pixel) */
    double cx = c.sx + a2 * 0.5, cy = c.sy + a3 * 0.5;   /* rect corners (screen) */
    double X0 = cx - a2, X1 = cx, Y0 = cy - a3, Y1 = cy;
    fquad q; q.sel = r->sel;
    q.sx[0]=X0; q.sy[0]=Y0; q.sx[1]=X1; q.sy[1]=Y0;
    q.sx[2]=X0; q.sy[2]=Y1; q.sx[3]=X1; q.sy[3]=Y1;
    for (int k = 0; k < 4; k++) q.sz[k] = (float)c.m3;
    q.cz = (float)c.m3 * PART_VZ_WORLD4;        /* world/4 -> full-world */
    decode_material(x->cpu, r->pp, r->mclass, &q);
    s_fq[s_fq_n++] = q;
}

/** @brief Builds a sel-4 HUD egg-tally sparkle, a LINE_G between two view-space points (block_80057954).
 *
 * The .s loads the stored coordinates as the GTE translation with vertex
 * (0, 0, 0), so both endpoints project with no camera rotation through the
 * canonical view: EP0 = (s16 A.lo, A.hi, B.lo), EP1 = (s16 B.hi, C.lo,
 * C.hi). Culls on EP0's SZ window (64..2048) and screen rect; (SZ3 >> 5) -
 * lod is the line's ordering-table bucket, clamped to 0 at 80057A30 and not
 * a cull, which sel4_bucket_depth turns into the depth. EP1 is
 * far_endpoint_lerp'ed (signed decode) while the slot's phase interpolates.
 * @param x The build's view inputs.
 * @param r The record. */
static void part_build_egg_line(const part_ctx_t* x, const part_rec_t* r) {
    if (s_pline_n >= MAX_PLINE) return;
    const uint32_t A = r->A, B = r->B;
    uint32_t C = x->cpu->read_word(r->pp + 0x0C);
    double e1v[3] = { (double)(int16_t)(B >> 16), (double)(int16_t)(C & 0xFFFF), (double)(int16_t)(C >> 16) };
    part_prev_t pv;
    if (part_phase_prev(r->pp, &pv)) {
        const double p[3] = { (double)(int16_t)(pv.B >> 16),  (double)(int16_t)(pv.C & 0xFFFF),
                              (double)(int16_t)(pv.C >> 16) };
        const double c[3] = { (double)(int16_t)(pv.cB >> 16), (double)(int16_t)(pv.cC & 0xFFFF),
                              (double)(int16_t)(pv.cC >> 16) };
        far_endpoint_lerp(p, c, pv.alpha, e1v);
    }
    projres e0, e1;
    proj_view(x->ofx, x->ofy, &x->vc, (double)(int16_t)(A & 0xFFFF), (double)(int16_t)(A >> 16),
              (double)(int16_t)(B & 0xFFFF), &e0);
    proj_view(x->ofx, x->ofy, &x->vc, e1v[0], e1v[1], e1v[2], &e1);
    int sz3 = (int)sz3_sat(e0.m3);
    uint32_t col0 = x->cpu->read_word(r->pp + 0x10), col1 = x->cpu->read_word(r->pp + 0x14);
    if (sz3 <= 64 || sz3 >= 2048) return;        /* sel-4 SZ window 64..2048 */
    if (screen_rect_cull(e0.sx, e0.sy)) return;
    part_push_line(r->pp, &e0, &e1, col0, col1, sel4_bucket_depth(sz3, col1), 4, "line4");
}

/** @brief Builds a sel-5 HUD key-collected burst, an affine FT4 about a view-space centre (block_80057750).
 *
 * The corners are sel 2's, about the direct view-space translation TRX =
 * (s16 A.lo) * 4, TRY = (int32)A >> 14, TRZ = (s16 B.lo) * 4, projected
 * through the canonical H. Culls on TRZ <= 0 and on (TRZ >> 7) - lod < 0,
 * lod byte 1 of +0x10, which the .s takes to the loop tail.
 * @param x The build's view inputs.
 * @param r The record. */
static void part_build_key_burst(const part_ctx_t* x, const part_rec_t* r) {
    double TR0 = (double)((int32_t)(r->A << 16) >> 14);  /* X*4 */
    double TR1 = (double)((int32_t)r->A >> 14);          /* ~Y*4 (combined) */
    double TR2 = (double)((int32_t)(r->B << 16) >> 14);  /* Z*4 */
    if (TR2 <= 0.0) return;
    int sz3i = (int)(TR2 > 65535 ? 65535 : TR2);
    int lod = (int)((x->cpu->read_word(r->pp + 0x10) >> 8) & 0xFF);
    if (((sz3i >> 7) - lod) < 0) return;         /* sel-5 cull TRZ>>7 - lod */
    double a2, a3;
    part_spin_extents(x, r, 32, 1024.0, 0, &a2, &a3);
    double sz3 = TR2 > 65535 ? 65535 : TR2;
    /* HUD burst: canonical projection, the cap regime on the same H */
    double qd = part_cap(sz3, x->Hc, x->Hc) ? NGTE_DIV_CAP : x->Hc/sz3;
    fquad q; q.sel = r->sel;
    part_affine_quad(x, a2, a3, TR0, TR1, TR2, qd, &q);
    q.cz = (float)TR2;
    decode_material(x->cpu, r->pp, r->mclass, &q);
    s_fq[s_fq_n++] = q;
}

/** @brief Builds a sel >= 6 rotated-billboard FT4, the default particle class.
 *
 * The four corners are the centre plus the spin extents (ang_ofs 0, div
 * 4096) rotated in the GTE X-Z plane, each projected through the camera.
 * Culls on the corners' clamped SZ3 sum outside [512, 32768), on the LOD
 * (sum >> 7) - texsub_hi < 0, and on clip4_cull; the depth is a quarter of
 * the sum.
 * @param x The build's view inputs.
 * @param r The record. */
static void part_build_rotated(const part_ctx_t* x, const part_rec_t* r) {
    double eS, eC;
    part_spin_extents(x, r, 0, 4096.0, 1, &eS, &eC);
    const double cx[4] = { r->VYb-eS, r->VYb+eC, r->VYb-eC, r->VYb+eS };
    const double cz[4] = { r->VXb+eC, r->VXb+eS, r->VXb-eS, r->VXb-eC };
    fquad q; q.sel = r->sel;
    double zsum = 0;
    for (int k = 0; k < 4; k++) {
        projres pr;
        proj2(x->R, x->ofx, x->ofy, x->v, cx[k], r->VZb, cz[k], &pr);
        q.sx[k] = (float)pr.sx; q.sy[k] = (float)pr.sy; q.sz[k] = (float)pr.m3;
        zsum += sz3_sat(pr.m3);
    }
    if (zsum < 512.0 || zsum >= 32768.0) return;            /* depth-sum window */
    if ((((int)zsum) >> 7) - r->texsub_hi < 0) return;      /* LOD cull */
    if (clip4_cull(q.sx, q.sy)) return;                     /* 4-corner clip */
    q.cz = (float)(zsum / 4.0) * PART_VZ_WORLD4;  /* world/4 -> full-world */
    decode_material(x->cpu, r->pp, r->mclass, &q);
    s_fq[s_fq_n++] = q;
}

/* A sel builder: projects, culls and appends one live record. */
typedef void part_build_fn(const part_ctx_t* x, const part_rec_t* r);
/* The builder per sel byte below 6; every higher sel is the default class. */
static part_build_fn* const k_part_build[6] = {
    part_build_tile, part_build_streak, part_build_affine,
    part_build_rect, part_build_egg_line, part_build_key_burst,
};

/** @brief Rebuilds s_fq and s_pline from the g_Particles pool the .s walked.
 *
 * Walks up to 320 records of s_part_pool, 0x20 bytes each, stopping at a
 * sel byte 0xFF and skipping records with bit 7 set, and hands each live
 * record to its sel's builder (k_part_build, part_build_rotated for sel >=
 * 6), which projects it in float under the running view s_pv with the .s
 * cull tests of its emitter and appends a quad (sel 0, 2, 3, 5, >= 6) or a
 * line (sel 1, 4), until MAX_FQUAD or MAX_PLINE. World-class centre depths
 * are scaled by PART_VZ_WORLD4. While s_phase_now is set, spin, size,
 * half-extent and far-endpoint channels are interpolated between the slot's
 * snapshots. Reads gamestate only.
 * @param cpu The guest CPU state. */
static void particle_build(CPUState* cpu) {
    s_fq_n = 0; s_pline_n = 0; s_ppdbg_done = 0;
    ngte_view_build(cpu, &s_pv);
    part_ctx_t x;
    x.cpu = cpu;
    x.v = &s_pv;
    memcpy(x.R, s_pv.cam_M, sizeof x.R);
    /* Camera at FRACTIONAL /4 (not >>2): the particle positions (A/B) are int16
     * world/4 gameplay storage, and flooring the camera to /4 adds a
     * camera-correlated step to every particle under panning. Carrying the
     * camera's low 2 bits tracks the sub-tick camera (the view holds the
     * un-rounded interpolated camera when bracketed). */
    x.s7 = s_pv.cam[0] / 4.0; x.t8 = s_pv.cam[1] / 4.0; x.t9 = s_pv.cam[2] / 4.0;
    x.ofx = s_pv.ofx; x.ofy = s_pv.ofy;
    x.H = s_pv.H_eff;
    ngte_view_canon(&s_pv, &x.vc);   /* HUD bursts (sel 4/5) project where the game put them */
    x.Hc = s_pv.H_canon;             /* GTE limit regime on the GTE's H */
    x.qcap = NGTE_DIV_CAP * s_pv.s;  /* the capped quotient scales with the view */

    uint32_t pp = s_part_pool;                          /* pool the .s walked (hook-entry) */
    for (int i = 0; i < 320 && s_fq_n < MAX_FQUAD; i++, pp += 0x20) {
        uint32_t hdr = cpu->read_word(pp);
        int mtype = (int)((hdr >> 8) & 0xFF);           /* dispatch byte (sel) */
        if (mtype == 0xFF) break;                       /* end of pool (gp>>8==-1) */
        if (mtype & 0x80) continue;                     /* inactive slot (skip, not stop) */
        part_rec_t r;
        r.pp = pp; r.slot = i; r.sel = mtype;
        r.mclass = (int)(hdr & 0xFF);                   /* texture/clut index */
        r.A = cpu->read_word(pp + 4); r.B = cpu->read_word(pp + 8);
        r.VXb = (double)(int)(r.A & 0xFFFF) - x.s7;     /* GTE-Z */
        r.VYb = x.t8 - (double)(int)(r.A >> 16);        /* GTE-X */
        r.VZb = x.t9 - (double)(int)(r.B & 0xFFFF);     /* GTE-Y */
        r.texsub_hi = (int)(cpu->read_word(pp + 0x10) & 0xFFFF) >> 8;
        (mtype < 6 ? k_part_build[mtype] : part_build_rotated)(&x, &r);
    }
}

/** @brief Compares the native particle quads and lines against the captured .s prims.
 *
 * Rebuilds the pool, then prints one [part-ab] line: the FT4 quads (sel
 * other than 0) against the code 0x2C and 0x2E prims in order and by
 * nearest centroid, the sel 0 tiles against the LINE_F (0x40) prims by
 * vertex 0 and covered-pixel footprint, and the sel 1 lines against the
 * LINE_G (0x50) prims by endpoint. OPENPETE_PARTICLE_DEBUG adds the per-sel
 * counts and the first quad pairs.
 * @param cpu The guest CPU state. */
static void particle_compare(CPUState* cpu) {
    particle_build(cpu);
    int ft4 = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x2C || s_prim[p].code == 0x2E) ft4++;
    int natft4 = 0; for (int q = 0; q < s_fq_n; q++) if (s_fq[q].sel != 0) natft4++;  /* sel 0 = non-FT4 */
    int n = natft4 < ft4 ? natft4 : ft4;
    double worst = 0, sum = 0; int nv = 0, matched = 0, over4 = 0, fqi = 0;
    for (int p = 0; p < s_prim_n && matched < n; p++) {
        prim_t* pr = &s_prim[p];
        if (!(pr->code == 0x2C || pr->code == 0x2E)) continue;
        while (fqi < s_fq_n && s_fq[fqi].sel == 0) fqi++;     /* skip sel-0 tile quads */
        if (fqi >= s_fq_n) break;
        fquad* q = &s_fq[fqi++];
        double qw = 0;
        for (int k = 0; k < 4; k++) {
            double dx = q->sx[k] - pr->x[k], dy = q->sy[k] - pr->y[k];
            double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
            sum += d; nv++; if (d > qw) qw = d;
        }
        if (qw > worst) worst = qw;
        if (qw > 4.0) over4++;
        matched++;
    }
    /* order-independent NN: each native FT4 quad -> nearest prim FT4 by centroid
     * (robust to a cull-boundary count mismatch that cascades the ordered A/B). */
    double nnsum = 0; int nnn = 0;
    for (int q = 0; q < s_fq_n; q++) {
        if (s_fq[q].sel == 0) continue;
        double qcx = (s_fq[q].sx[0]+s_fq[q].sx[1]+s_fq[q].sx[2]+s_fq[q].sx[3])/4.0;
        double qcy = (s_fq[q].sy[0]+s_fq[q].sy[1]+s_fq[q].sy[2]+s_fq[q].sy[3])/4.0;
        double best = 1e9;
        for (int p = 0; p < s_prim_n; p++) {
            if (!(s_prim[p].code==0x2C||s_prim[p].code==0x2E)) continue;
            double pcx=(s_prim[p].x[0]+s_prim[p].x[1]+s_prim[p].x[2]+s_prim[p].x[3])/4.0;
            double pcy=(s_prim[p].y[0]+s_prim[p].y[1]+s_prim[p].y[2]+s_prim[p].y[3])/4.0;
            double d=(qcx-pcx<0?pcx-qcx:qcx-pcx)+(qcy-pcy<0?pcy-qcy:qcy-pcy);
            if (d<best) best=d;
        }
        if (best<1e8) { nnsum+=best; nnn++; }
    }
    int ns2=0,ns3=0,ns5=0,ns6=0; for (int q=0;q<s_fq_n;q++){if(s_fq[q].sel==2)ns2++;else if(s_fq[q].sel==3)ns3++;else if(s_fq[q].sel==5)ns5++;else if(s_fq[q].sel>=6)ns6++;}
    int npl1=0,npl4=0; for (int t=0;t<s_pline_n;t++){if(s_pline[t].sel==4)npl4++;else npl1++;}
    if (getenv("OPENPETE_PARTICLE_DEBUG")) fprintf(stderr, "   [sel] nat sel2=%d sel3=%d sel5=%d sel6=%d line1=%d line4=%d\n", ns2, ns3, ns5, ns6, npl1, npl4);
    int nat_sel0 = 0; for (int q = 0; q < s_fq_n; q++) if (s_fq[q].sel == 0) nat_sel0++;
    /* sel-0 GEOMETRY (closes the count-only blind spot that hid the 1px-sliver
     * bug): the .s emits each sel-0 as a LINE_F (code 0x40), kept raw - vertex0
     * is raw[2] packed (Y<<16)|X. NN-match each native sel-0 quad anchor
     * (sx[0],sy[0]) to the nearest .s line vertex0 and report the distance. */
    int prim_line = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x40) prim_line++;
    double t0sum = 0, t0worst = 0; int t0n = 0, t0fpmis = 0;
    for (int q = 0; q < s_fq_n; q++) {
        if (s_fq[q].sel != 0) continue;
        double ax = s_fq[q].sx[0], ay = s_fq[q].sy[0], best = 1e9; int bp = -1;
        for (int p = 0; p < s_prim_n; p++) {
            if (s_prim[p].code != 0x40) continue;
            double lx = (double)(int16_t)(s_prim[p].raw[2] & 0xFFFF);
            double ly = (double)(int16_t)(s_prim[p].raw[2] >> 16);
            double d = (ax-lx<0?lx-ax:ax-lx) + (ay-ly<0?ly-ay:ay-ly);
            if (d < best) { best = d; bp = p; }
        }
        if (bp >= 0) {
            t0sum += best; if (best > t0worst) t0worst = best; t0n++;
            /* FOOTPRINT check - the 1px-sliver bug was a WIDTH error at the
             * correct position, which a position NN alone misses. Compare the
             * native quad's covered-pixel extent to the .s line's: line covers
             * pixels v0..v1 inclusive = |Δ|+1; the native quad covers sx[1]-sx[0]
             * (ngte_line_quad's 2 px for the SX..SX+1 line). */
            int v0x=(int16_t)(s_prim[bp].raw[2]&0xFFFF), v1x=(int16_t)(s_prim[bp].raw[3]&0xFFFF);
            int v0y=(int16_t)(s_prim[bp].raw[2]>>16),    v1y=(int16_t)(s_prim[bp].raw[3]>>16);
            int line_w=(v1x>v0x?v1x-v0x:v0x-v1x)+1, line_h=(v1y>v0y?v1y-v0y:v0y-v1y)+1;
            int nat_w=(int)(s_fq[q].sx[1]-s_fq[q].sx[0]+0.5), nat_h=(int)(s_fq[q].sy[2]-s_fq[q].sy[0]+0.5);
            if (nat_w != line_w || nat_h != line_h) t0fpmis++;
        }
    }
    /* sel-1 gouraud LINE_G (code 0x50) A/B: NN native streak endpoints -> nearest
     * .s 0x50 line endpoint (raw[2]=xy0, raw[4]=xy1, packed (Y<<16)|X). */
    int prim_gline = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x50) prim_gline++;
    double l1sum = 0, l1worst = 0; int l1n = 0;
    for (int t = 0; t < s_pline_n; t++) {
        double e[2][2] = { {s_pline[t].ax, s_pline[t].ay}, {s_pline[t].bx, s_pline[t].by} };
        for (int k = 0; k < 2; k++) {
            double best = 1e9;
            for (int p = 0; p < s_prim_n; p++) {
                if (s_prim[p].code != 0x50) continue;
                int idx[2] = {2, 4};
                for (int m = 0; m < 2; m++) {
                    double lx = (double)(int16_t)(s_prim[p].raw[idx[m]] & 0xFFFF);
                    double ly = (double)(int16_t)(s_prim[p].raw[idx[m]] >> 16);
                    double d = (e[k][0]-lx<0?lx-e[k][0]:e[k][0]-lx) + (e[k][1]-ly<0?ly-e[k][1]:e[k][1]-ly);
                    if (d < best) best = d;
                }
            }
            if (best < 1e8) { l1sum += best; if (best > l1worst) l1worst = best; l1n++; }
        }
    }
    fprintf(stderr, "[part-ab f=%u] FT4 native=%d prim=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx over4=%d/%d nnCtr=%.1f  sel0:nat=%d line=%d nn=%.1f/%.1f fpMis=%d  sel1:nat=%d gline=%d nn=%.1f/%.1f  sel4:nat=%d sel5:nat=%d\n",
            openpete_frame, natft4, ft4, n, nv?sum/nv:0, worst, over4, n, nnn?nnsum/nnn:0,
            nat_sel0, prim_line, t0n?t0sum/t0n:0, t0worst, t0fpmis,
            s_pline_n, prim_gline, l1n?l1sum/l1n:0, l1worst, npl4, ns5);
    if (getenv("OPENPETE_PARTICLE_DEBUG")) {
        for (int p = 0, c = 0; p < s_prim_n && c < 3 && c < s_fq_n; p++) {
            if (!(s_prim[p].code==0x2C||s_prim[p].code==0x2E)) continue;
            fprintf(stderr, "   sel%d prim:(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)  nat:(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n",
                    s_fq[c].sel,
                    s_prim[p].x[0],s_prim[p].y[0],s_prim[p].x[1],s_prim[p].y[1],
                    s_prim[p].x[2],s_prim[p].y[2],s_prim[p].x[3],s_prim[p].y[3],
                    s_fq[c].sx[0],s_fq[c].sy[0],s_fq[c].sx[1],s_fq[c].sy[1],
                    s_fq[c].sx[2],s_fq[c].sy[2],s_fq[c].sx[3],s_fq[c].sy[3]);
            c++;
        }
    }
}

/* ---- native feed ------------------------------------------------------- */
/** @brief Returns the native scene layer a g_Particles sel feeds its primitives into.
 *
 * sel 5, the key-collected FT4 burst (block_80057750), and sel 4, the
 * egg-tally LINE_G burst (block_80057954), hold view-space positions around
 * a HUD element, so their depth is HUD-space Z, which is not comparable with
 * world depth; the .s links them into a near g_WorldOT particle bucket
 * ((Z>>7)-lod for sel 5, (SZ3>>5)-lod for sel 4) that paints over the world.
 * Both take the flat HUD layer (OPENPETE_SCENE_HUD: depth-test off, drawn
 * after the world and, outside the level-transition flyby, the player, and
 * under the HUD3D icons). The egg counter the sel-4 lines surround is a 2D
 * sprite (func_8001919C) linked into g_HudOT, which the game draws after
 * g_WorldOT, so it covers the lines; the HUD layer draws its untextured
 * particle triangles before its 2D content to keep that order. Every other
 * sel is world geometry and takes WORLD at its true view-Z, as does a sel
 * outside the table.
 * @param sel The record's sel, header bits 15:8.
 * @return An OPENPETE_SCENE_* layer. */
static int part_sel_layer(int sel) {
    static const int k_layer[] = {
        OPENPETE_SCENE_WORLD,   /* 0: TILE / dot */
        OPENPETE_SCENE_WORLD,   /* 1: motion-streak LINE_G */
        OPENPETE_SCENE_WORLD,   /* 2: affine billboard */
        OPENPETE_SCENE_WORLD,   /* 3: depth-cued screen rect */
        OPENPETE_SCENE_HUD,     /* 4: egg-tally LINE_G burst */
        OPENPETE_SCENE_HUD,     /* 5: key-collected FT4 burst */
    };
    return (sel >= 0 && sel < (int)(sizeof k_layer / sizeof k_layer[0]))
        ? k_layer[sel] : OPENPETE_SCENE_WORLD;
}

/** @brief Feeds a screen-space line between two GTE float endpoints as the two triangles of ngte_line_quad's quad.
 *
 * The quad covers the pixels the GPU draws for the line (centre offset 0).
 * Untextured, at one depth, gouraud along the line: corners 0 and 2 take
 * rgb0, 1 and 3 rgb1.
 * @param ax Endpoint a's screen X, PSX pixels.
 * @param ay Endpoint a's screen Y.
 * @param bx Endpoint b's screen X.
 * @param by Endpoint b's screen Y.
 * @param rgb0 Endpoint a's colour, low 24 bits.
 * @param rgb1 Endpoint b's colour, low 24 bits.
 * @param depth The scene depth.
 * @param blend The native blend code, 0 for opaque.
 * @param layer The OPENPETE_SCENE_* layer. */
static void part_feed_line_quad(float ax, float ay, float bx, float by, uint32_t rgb0,
                                uint32_t rgb1, float depth, int blend, int layer) OP_REQUIRES_SHARED(sim) {
    static const int split[2][3] = { {0,1,2}, {1,3,2} };
    float cx[4], cy[4];
    ngte_line_quad(ax, ay, bx, by, 0.0f, cx, cy);
    const uint32_t cc[4] = { rgb0, rgb1, rgb0, rgb1 };
    for (int s = 0; s < 2; s++) {
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            const int j = split[s][k];
            st.v[k].sx = cx[j]; st.v[k].sy = cy[j]; st.v[k].z = depth; st.v[k].pvz = depth;
            st.v[k].rgba = (cc[j] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.blend = blend;
        openpete_native_scene_add_tri(&st, layer, 0);
    }
}

/** @brief Builds the g_Particles pool walk and feeds its quads and lines into the native scene.
 *
 * Lines (sel 1 world streaks, sel 4 HUD egg bursts) become the quad
 * ngte_line_quad builds over the pixels the GPU draws, gouraud along the
 * line, untextured. Quads (sel 0
 * tiles, sel 2, 3 and 5 billboards) become two triangles with the decoded
 * material and blend. Each primitive takes one depth, its centre or first
 * endpoint view-Z, because particles are small billboards, and the layer
 * part_sel_layer names for its sel. Every triangle carries its
 * OPENPETE_TAG_PARTICLE tag. Glows and sparkles feed through
 * their own functions. Display-only: the .s already ran and wrote every
 * verdict byte. OPENPETE_PARTICLE_MATDBG=<n> prints each quad's material for
 * three openpete_frame values starting at n.
 * @param cpu The guest CPU state. */
static void particle_feed_native(CPUState* cpu) OP_REQUIRES(sim) {
    particle_build(cpu);
    if (s_fq_n <= 0 && s_pline_n <= 0) return;
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);   /* append after world + actors */
    int split[2][3] = { {0,1,2}, {1,3,2} };              /* FT4 -> 2 tris */
    /* sel-1 and sel-4 gouraud lines */
    for (int i = 0; i < s_pline_n; i++) {
        const pline* l = &s_pline[i];
        openpete_native_scene_set_tag(
            OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_LINE, l->sel, 0));
        part_feed_line_quad(l->ax, l->ay, l->bx, l->by, l->rgb0, l->rgb1,
                            openpete_native_scene_viewz(openpete_worldvz_full(l->cz)),
                            l->blend, part_sel_layer(l->sel));
    }
    openpete_native_scene_set_tag(0);
    if (s_fq_n <= 0) return;
    /* OPENPETE_PARTICLE_MATDBG=<frame>: one line per emitted quad - sel/blend/
     * rgb/texture - to attribute a missing-or-wrong-blend particle (e.g. the
     * dragon-fragment smoke puffs) to its material decode. */
    { static long s_mdbg = -2;
      if (s_mdbg < -1) { const char* e = getenv("OPENPETE_PARTICLE_MATDBG");
                         s_mdbg = (e && *e) ? atol(e) : -1; }
      if (s_mdbg >= 0 && openpete_frame >= (unsigned)s_mdbg && openpete_frame < (unsigned)s_mdbg + 3) {
          for (int i = 0; i < s_fq_n; i++) {
              fquad* q = &s_fq[i];
              fprintf(stderr, "[part-mat f=%u] sel=%d blend=%d tex=%d rgb=%06X "
                      "tp=(%d,%d) clut=(%d,%d) bpp=%d ctr=(%.0f,%.0f) cz=%.0f\n",
                      openpete_frame, q->sel, q->blend, q->textured, q->rgb,
                      q->tpx, q->tpy, q->clx, q->cly, q->bpp,
                      (q->sx[0]+q->sx[1]+q->sx[2]+q->sx[3])/4.0,
                      (q->sy[0]+q->sy[1]+q->sy[2]+q->sy[3])/4.0, q->cz);
              if (q->blend == 3)
                  fprintf(stderr, "   corners (%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) uv (%d,%d)(%d,%d)(%d,%d)(%d,%d)\n",
                          q->sx[0],q->sy[0],q->sx[1],q->sy[1],q->sx[2],q->sy[2],q->sx[3],q->sy[3],
                          q->u[0],q->v[0],q->u[1],q->v[1],q->u[2],q->v[2],q->u[3],q->v[3]);
          }
      } }
    for (int i = 0; i < s_fq_n; i++) {
        fquad* q = &s_fq[i];
        openpete_native_scene_set_tag(
            OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_QUAD, q->sel, 0));
        /* No depth pull for the subtractive smoke: the burst point is the
         * occluder's NEAREST surface from the camera, so true depth never
         * blocks the puffs. The subtract-world bucket is drawn after the ADD
         * group renderer-side, which keeps the puffs from being eaten. */
        float depth = openpete_native_scene_viewz(openpete_worldvz_full(q->cz));
        for (int s = 0; s < 2; s++) {
            openpete_scene_tri st = {0};
            for (int k = 0; k < 3; k++) {
                int j = split[s][k];
                st.v[k].sx = q->sx[j]; st.v[k].sy = q->sy[j]; st.v[k].z = depth; st.v[k].pvz = depth;
                st.v[k].u = (float)q->u[j]; st.v[k].v = (float)q->v[j];
                st.v[k].rgba = (q->rgb & 0xFFFFFFu) | 0xFF000000u;
            }
            st.textured = q->textured;
            st.tpage_x = q->tpx; st.tpage_y = q->tpy;
            st.clut_x = q->clx; st.clut_y = q->cly; st.bpp = q->bpp;
            st.blend = q->blend;
            openpete_native_scene_add_tri(&st, part_sel_layer(q->sel), 0);
        }
    }
    openpete_native_scene_set_tag(0);
}

/* ---- glow rings (func_800580F4) - native float walk + feed + A/B ----------- *
 * Decoded from the recompiled C (generated/PSX.EXE_full.c block_800580F4). Each
 * active glow (D_80078800[16], stride 0x24) is a gouraud TRIANGLE FAN: a bright
 * centre (the glow colour) fading to BLACK at a perspective-scaled ring, drawn
 * ADDITIVE (the .s emits a 0xE1000220 draw-mode packet = ABR mode 1, then N-1
 * G3-STP tris, code 0x32). The ring template (unk_0x04, default D_8006E308) is a
 * static 17-point ellipse (96 wide x 60 tall - pre-compensated for tall PSX
 * pixels, so ~circular on the 4:3 display); unk_0x00 is the ring point count.
 *
 * Differences from the particle/sparkle swizzle: the glow uses RAW camera world
 * coords (NO >>2 - MobyPos is full-scale world units), and a per-glow LOD shift
 * that cancels out of SX/SY and persize, so the float walk skips it (the int16
 * floor it introduces is exactly what we're improving on). All gamestate-derived
 * (pool + static ring template + colour); determinism-safe by construction (the
 * .s ran headless; glows write no verdict byte). */
typedef struct {
    float sx[3], sy[3];      /* fan tri: v0=centre, v1=prev ring, v2=this ring */
    uint32_t rgb[3];         /* v0=glow colour, v1/v2=black (the fade) */
    float cz;                /* centre view-Z (depth) */
} gtri;
#define MAX_GTRI 4096
static gtri s_gtri[MAX_GTRI]; static int s_gtri_n;

/** @brief Rebuilds s_gtri from the D_80078800 glow records.
 *
 * Each record with a positive ring count, an owner MobyPos and a ring
 * template is projected from raw world coordinates plus its offset. Records
 * behind the camera or with a non-positive OT depth index are skipped. The
 * ring is at most 64 template points scaled by size * 4096 / MAC3, with the
 * scale capped at 4096 as the GTE IR0 saturates, and each consecutive ring
 * pair becomes one fan triangle (centre in the glow colour, ring vertices
 * black). While s_phase_now is set, size, colour and offset are interpolated
 * between the slot's snapshots when owner, template and count match.
 * @param cpu The guest CPU state. */
static void glow_build(CPUState* cpu) {
    s_gtri_n = 0;
    ngte_view_build(cpu, &s_pv);
    const ngte_view* v = &s_pv;
    int R[3][3]; memcpy(R, v->cam_M, sizeof R);
    /* RAW world units (no >>2); fractional interpolated camera when bracketed
     * (sub-tick floor fix), else floored RAM. */
    double camX = v->cam[0], camY = v->cam[1], camZ = v->cam[2];
    double ofx = v->ofx, ofy = v->ofy;

    int gdbg_done = 0;                                /* one glow-phase line per walk */
    for (int i = 0; i < 16 && s_gtri_n < MAX_GTRI; i++) {
        uint32_t g = D_GLOWS + (uint32_t)i*0x24;
        int count = (int)cpu->read_word(g + 0x00);    /* unk_0x00 = ring point count */
        if (count <= 0) continue;                     /* inactive slot */
        uint32_t mpp = cpu->read_word(g + 0x08);      /* MobyPos* */
        if (!mpp) continue;
        uint32_t tmpl = cpu->read_word(g + 0x04);     /* ring template (int32 x,y pairs) */
        if (!tmpl) continue;
        int offx = (int)cpu->read_word(g + 0x14);     /* PosOffset (animatable) */
        int offy = (int)cpu->read_word(g + 0x18);
        int offz = (int)cpu->read_word(g + 0x1C);
        int px = (int)cpu->read_word(mpp+0);          /* owner pos (rides the moby bracket) */
        int py = (int)cpu->read_word(mpp+4);
        int pz = (int)cpu->read_word(mpp+8);
        int size = (int)cpu->read_word(g + 0x10);
        int bias = (int)cpu->read_word(g + 0x20);
        uint32_t color = cpu->read_word(g + 0x0C) & 0xFFFFFF;

        /* Sub-tick phase: lerp size/colour/offset between the slot's prev/curr
         * interp-objs snapshots (captured in interp_regions.c). The Sparx
         * aura RAMPS size +32/tick over ~18 ticks on spawn (censused) and
         * stepped at 29.913 Hz. Guards: same owner (+0x08),
         * template (+0x04) and ring count - a slot reused for a different glow
         * snaps. Colour is additive light, so a per-byte linear lerp is safe. */
        double sizef = (double)size;
        double offxf = (double)offx, offyf = (double)offy, offzf = (double)offz;
        uint32_t colorL = color;
        if (s_phase_now) {
            const openpete_interp_obj_t* o = openpete_interp_objs_find(g);
            if (o && o->have_prev) {
                uint32_t pcnt, ptmpl, pmpp;
                memcpy(&pcnt,  o->prev + 0x00, 4);
                memcpy(&ptmpl, o->prev + 0x04, 4);
                memcpy(&pmpp,  o->prev + 0x08, 4);
                uint32_t ccnt2, ctmpl2, cmpp2;
                memcpy(&ccnt2,  o->curr + 0x00, 4);
                memcpy(&ctmpl2, o->curr + 0x04, 4);
                memcpy(&cmpp2,  o->curr + 0x08, 4);
                /* Guards + forward endpoints from the TABLE pair (see the
                 * sparkle note): prev->table-curr, never prev->live. */
                double a;
                if (pcnt == ccnt2 && (int32_t)ccnt2 > 0 && ptmpl == ctmpl2 &&
                    pmpp == cmpp2 && part_slot_alpha(o, &a)) {
                    uint32_t pcol, psz, po14, po18, po1c;
                    uint32_t ccol, csz, co14, co18, co1c;
                    memcpy(&pcol, o->prev + 0x0C, 4);
                    memcpy(&psz,  o->prev + 0x10, 4);
                    memcpy(&po14, o->prev + 0x14, 4);
                    memcpy(&po18, o->prev + 0x18, 4);
                    memcpy(&po1c, o->prev + 0x1C, 4);
                    memcpy(&ccol, o->curr + 0x0C, 4);
                    memcpy(&csz,  o->curr + 0x10, 4);
                    memcpy(&co14, o->curr + 0x14, 4);
                    memcpy(&co18, o->curr + 0x18, 4);
                    memcpy(&co1c, o->curr + 0x1C, 4);
                    sizef = (double)(int32_t)psz  + ((double)(int32_t)csz  - (double)(int32_t)psz)  * a;
                    if (sizef < 0.0) sizef = 0.0;   /* backward extrapolation on a spawn ramp */
                    offxf = (double)(int32_t)po14 + ((double)(int32_t)co14 - (double)(int32_t)po14) * a;
                    offyf = (double)(int32_t)po18 + ((double)(int32_t)co18 - (double)(int32_t)po18) * a;
                    offzf = (double)(int32_t)po1c + ((double)(int32_t)co1c - (double)(int32_t)po1c) * a;
                    double cr = lerp_b((int)(pcol & 0xFF),         (int)(ccol & 0xFF),         a);
                    double cg = lerp_b((int)((pcol >> 8) & 0xFF),  (int)((ccol >> 8) & 0xFF),  a);
                    double cb = lerp_b((int)((pcol >> 16) & 0xFF), (int)((ccol >> 16) & 0xFF), a);
                    colorL = (uint32_t)(cr + 0.5) | ((uint32_t)(cg + 0.5) << 8)
                           | ((uint32_t)(cb + 0.5) << 16);
                    if (glow_anim_dbg() && !gdbg_done) { gdbg_done = 1;
                        fprintf(stderr, "[glow-phase f=%u] slot=%d lerp size=%.2f col=%06X (raw %d/%06X)\n",
                                openpete_frame, i, sizef, colorL, size, color); }
                }
            }
        }

        /* OPENPETE_GLOW_ANIM_DBG: census - does the game ANIMATE glow records
         * per tick (size/colour/offset pulse)? Logs RAW values on CHANGE only;
         * a change stream at ~tick rate = a stepping channel (the Sparx
         * aura size ramps +32/tick on spawn - hence the phase lerp
         * above). */
        if (glow_anim_dbg()) {
            static uint32_t s_glast[16][6]; static int s_gseen[16];
            uint32_t cur[6] = { (uint32_t)count, (uint32_t)size, color,
                                (uint32_t)offx, (uint32_t)offy, (uint32_t)offz };
            if (!s_gseen[i] || memcmp(cur, s_glast[i], sizeof cur) != 0) {
                fprintf(stderr, "[glow-anim f=%u] slot=%d cnt=%u size=%u col=%06X off=(%d,%d,%d)%s\n",
                        openpete_frame, i, cur[0], cur[1], cur[2],
                        (int32_t)cur[3], (int32_t)cur[4], (int32_t)cur[5],
                        s_gseen[i] ? "" : " (new)");
                memcpy(s_glast[i], cur, sizeof cur); s_gseen[i] = 1;
            }
        }

        /* swizzle like particles but in raw world units: GTE-X=camY-py,
         * GTE-Y=camZ-pz, GTE-Z=px-camX; TR=0 through the camera matrix. */
        projres c; proj2(R, ofx, ofy, v, camY - ((double)py + offyf),
                         camZ - ((double)pz + offzf), ((double)px + offxf) - camX, &c);
        double mac3 = c.m3;
        if (mac3 <= 0.0) continue;                    /* behind camera */
        double depthidx = mac3 / 128.0 + bias;        /* the .s OT-depth cull (a3<=0) */
        if (depthidx <= 0.0) continue;
        double persize = sizef * 4096.0 / mac3;       /* IR0 = size*4096/Z */
        /* GTE IR0 saturates at 0x1000 on hardware - without the clamp a
         * near-camera glow (mac3 -> small) spikes its ring across the
         * screen where the PSX caps it at template scale. Runahead makes
         * the near pass MORE common: the committed camera trails the led
         * Sparx by N extra ticks, so he crosses the camera plane closer. */
        if (persize > 4096.0) persize = 4096.0;

        int nc = count > 64 ? 64 : count;
        double rx[64], ry[64];
        for (int j = 0; j < nc; j++) {
            int tx = (int)cpu->read_word(tmpl + (uint32_t)j*8 + 0);  /* template offset (int32) */
            int ty = (int)cpu->read_word(tmpl + (uint32_t)j*8 + 4);
            rx[j] = c.sx + persize * tx / 4096.0;     /* GPF: centre + (IR0*tmpl)>>12 */
            ry[j] = c.sy + persize * ty / 4096.0;
        }
        for (int j = 1; j < nc && s_gtri_n < MAX_GTRI; j++) {
            gtri* t = &s_gtri[s_gtri_n++];
            t->sx[0]=(float)c.sx;   t->sy[0]=(float)c.sy;   t->rgb[0]=colorL;  /* centre */
            t->sx[1]=(float)rx[j-1];t->sy[1]=(float)ry[j-1];t->rgb[1]=0;       /* prev ring */
            t->sx[2]=(float)rx[j];  t->sy[2]=(float)ry[j];  t->rgb[2]=0;       /* this ring */
            t->cz = (float)mac3;
            if (streak_dbg()) streak_log("glow", g, c.sx, c.sy, rx[j], ry[j]);
        }
    }
}

/** @brief Builds the glow ring fans and feeds them into the native scene as additive gouraud triangles.
 *
 * Untextured, blend 2 (ABR mode 1). Display-only: the .s already ran and
 * wrote every verdict byte.
 *
 * Each triangle carries koff = centre MAC3 + 0.5, the fan's ordering-table
 * key in world units ((MAC3 >> 7) + rec[+0x20] in r_particles.s:988-990,
 * the field being 0), and the OPENPETE_SCENE_RANK_AURA sentinel, which puts
 * its fragments on the key contest's aura arm. The fan is ordered against
 * the bodies it overlaps by the table's own slots (the owner's bucket minus
 * m_DepthOffset, 4 on Sparx, the key at 8001F810..F828 and the offset
 * packed at 8001F464..F46C; the avatar's bucket minus sortDepth) while its
 * stored depth stays the true view-Z, so terrain still occludes it by
 * geometry. One stored depth cannot do both: a fan pushed behind its owner
 * in world units sinks into the ground the table draws under it.
 * @param cpu The guest CPU state. */
static void glow_feed_native(CPUState* cpu) OP_REQUIRES(sim) {
    glow_build(cpu);
    if (s_gtri_n <= 0) return;
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(
        OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_GLOW, 0, 0));
    for (int i = 0; i < s_gtri_n; i++) {
        gtri* t = &s_gtri[i];
        float depth = openpete_native_scene_viewz(openpete_worldvz_full(t->cz));
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = t->sx[k]; st.v[k].sy = t->sy[k]; st.v[k].z = depth; st.v[k].pvz = depth;
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (t->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 2;                                 /* ABR mode 1 (additive) */
        st.koff = t->cz + 0.5f;                       /* the link key + sentinel */
        st.rank = OPENPETE_SCENE_RANK_AURA;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Compares the native glow fan against the renderer's captured G3 semi-transparent prims (code 0x32).
 *
 * Rebuilds the fan, then matches every native vertex to its nearest .s G3
 * vertex by Manhattan distance, independent of order, which checks both the
 * centre projection and the ring corners. Prints one [glow-ab] line with
 * the triangle and prim counts, the mean and the worst distance.
 * @param cpu The guest CPU state. */
static void glow_compare(CPUState* cpu) {
    glow_build(cpu);
    int prim_g3 = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x32) prim_g3++;
    double sum = 0, worst = 0; int nn = 0;
    for (int t = 0; t < s_gtri_n; t++) for (int k = 0; k < 3; k++) {
        double ax = s_gtri[t].sx[k], ay = s_gtri[t].sy[k], best = 1e9;
        for (int p = 0; p < s_prim_n; p++) {
            if (s_prim[p].code != 0x32) continue;
            for (int m = 0; m < 3; m++) {
                double dx = ax - s_prim[p].x[m], dy = ay - s_prim[p].y[m];
                double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
                if (d < best) best = d;
            }
        }
        if (best < 1e8) { sum += best; if (best > worst) worst = best; nn++; }
    }
    fprintf(stderr, "[glow-ab f=%u] native_tri=%d prim_g3=%d nn=%.2f worst=%.1fpx\n",
            openpete_frame, s_gtri_n, prim_g3, nn?sum/nn:0, worst);
}

/* ---- sparkle twinkles (func_800584C4) - native float walk + feed + A/B ----- *
 * Decoded from the recompiled C (block_800584C4). Each live sparkle (g_Sparkles[8],
 * stride 0x18) is a "twinkle cross" = TWO LINE_F prims (code 0x40, opaque white):
 * T0->T3 and T1->T2, the two diagonals. Tips (view space, VZ=0):
 *   T0=(-ec,-es) T1=(es,-ec) T2=(-es,ec) T3=(ec,es),
 *   ec=(scaledFade·cos[ang])>>19, es=(scaledFade·sin[ang])>>19,
 *   ang=m_0x0E (the spin seed), scaledFade=(256−fade8)·m_Fade,
 *   fade8=|m_Speed−newLife|·256/m_Speed.
 * They are transformed by a uniform matrix diag(a3,a3,a3), a3=max(centreMAC3,4096),
 * TR = centre·2 (the centre's view-space MAC), then projected.
 *
 * VERDICT-SAFE by construction: the `.s` ran headless (realfn) - it decremented
 * m_Life and killed off-screen/expired sparkles by writing m_Life=0. So we read
 * the pool AFTER the `.s` and **render exactly the sparkles with post-`.s`
 * m_Life>0** (which is `newLife` - the value the `.s` used for the fade), no
 * separate cull needed. The native lines are emitted as thin quads (the scene is
 * triangles-only), like the sel-0 line footprint. */
typedef struct {
    float ax, ay, bx, by;    /* line endpoints (centerline) */
    float cz;                /* centre view-Z (depth) */
    uint32_t rgb;            /* m_Color (flat) */
} sline;
#define MAX_SLINE 64
static sline s_sline[MAX_SLINE]; static int s_sline_n;

/* Sparkle STYLE channel: the pad byte after m_Color's rgb (+0x13) is
 * written by nothing in the game - a mod that hooks the spawner
 * (SpawnMobySparkle) can stamp a style id there per sparkle and the
 * NATIVE renderer restyles it (the PSX path still draws the stock
 * cross, same posture as mod textures). Byte layout: low nibble =
 * style, high nibble = hue index+1 (0 = no hue). Styles:
 * 0 = stock twinkle cross;
 * 1 = 4-point star flare (the Spyro 3 gem-glint look): filled concave
 * star through the SAME centre/pulse/spin pipeline, additive blend.
 * With no hue: white-hot core fading to m_Color at the tips (legacy).
 * With a hue: the full S3 two-mode glint (traced from SCUS-94467
 * func_8002F098) - S3 does NOT lerp; it switches prim style at
 * viewZ 0x4000: far = the plain cross, pale-tinted (word0, opaque);
 * near = the additive gouraud star, pale core (word0) -> deep hue at
 * the spike tips (word1) -> near-black at the waists (word2, S3's
 * ring verts). */
typedef struct {
    float sx[9], sy[9];      /* centre + 8 outline points (tip/waist x4) */
    float cz;
    uint32_t rgb;
    int hue;                 /* 0 = legacy white flare; 1..6 = k_s3_glint row+1 */
} sstar;
#define MAX_SSTAR 16
static sstar s_sstar[MAX_SSTAR]; static int s_sstar_n;

/* S3 gem-glint colour table, D_80066B64 in SCUS-94467: per hue
 * {centre + far-line, spike tip, ring}.
 * Packed 0xBBGGRR to match the sparkle m_Color/rgba convention. */
#define S3G(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16))
static const uint32_t k_s3_glint[6][3] = {
    { S3G(224,160,128), S3G(64,16, 0), S3G(16, 0, 0) },  /* red   (1)  */
    { S3G(160,224,160), S3G( 0,64, 0), S3G( 0,16, 0) },  /* green (2)  */
    { S3G(160,128,224), S3G(32, 0,64), S3G( 8, 0,16) },  /* blue  (5)  */
    { S3G(208,208,128), S3G(48,48, 0), S3G(16,16, 0) },  /* gold  (10) */
    { S3G(224,128,192), S3G(64, 0,32), S3G(32, 0,16) },  /* pink  (25) */
    { S3G(128,128,128), S3G( 0, 0, 0), S3G( 0, 0, 0) },  /* misc/grey  */
};
/* S3 near/far switch: viewZ < 0x4000 world units -> star; c.m3 is
 * view-Z in world/4, so the threshold is 0x1000 in c.m3 units. */
#define S3_GLINT_NEAR_M3 4096.0

/** @brief Walks g_Sparkles and builds each live sparkle's twinkle cross lines or style-1 star.
 *
 * Reads the pool after the .s ran, so a slot with m_Life > 0 is exactly a
 * sparkle the .s drew. A cross is two lines (T0 to T3, T1 to T2) into
 * s_sline; a style-1 sparkle (low nibble of the pad byte +0x13) nearer
 * than S3_GLINT_NEAR_M3, or with no hue, is an 8-point star into s_sstar.
 *
 * A sparkle's position is static for its life; its animation phase steps
 * per tick: m_Life (pulse scale) decrements and m_0x0E (spin) advances.
 * While s_phase_now is set (the re-extract pass), life and spin are
 * interpolated between the slot's interp-objs prev and curr snapshots at
 * the slot's own alpha, and the fade math runs in float (fade8,
 * scaledFade, ngte_sincos_f); a respawn, a moved position or a changed
 * m_Speed snaps. With the latch clear the canonical inline build stays
 * integer and byte-exact for the sparkle A/B. Sparkle RAM is never
 * written.
 * @param cpu The guest CPU state. */
static void sparkle_build(CPUState* cpu) {
    s_sline_n = 0;
    s_sstar_n = 0;
    ngte_view_build(cpu, &s_pv);
    const ngte_view* v = &s_pv;
    int R[3][3]; memcpy(R, v->cam_M, sizeof R);
    /* RAW (the .s shifts the diff >>2); fractional interpolated camera when
     * bracketed (sub-tick floor fix), else floored RAM. */
    double camX = v->cam[0], camY = v->cam[1], camZ = v->cam[2];
    double ofx = v->ofx, ofy = v->ofy;
    double H   = v->H_eff;
    const double Hc = v->H_canon, qcap = NGTE_DIV_CAP * v->s;  /* GTE limit regime on the GTE's H; capped quotient scales with the view */

    for (int i = 0; i < 8 && s_sline_n + 2 <= MAX_SLINE; i++) {
        uint32_t sp = G_SPARKLES + (uint32_t)i*0x18;
        uint32_t w3 = cpu->read_word(sp + 0x0C);
        int newLife = (int)(w3 & 0xFF);              /* post-.s m_Life (== newLife) */
        if (newLife <= 0) continue;                  /* killed/expired by the .s -> skip */
        int speed = (int)((w3 >> 8) & 0xFF);
        if (speed <= 0) continue;
        uint32_t w5 = cpu->read_word(sp + 0x14);
        int fade = (int)(w5 & 0xFF);                 /* m_Fade (+0x14) */
        uint32_t color = cpu->read_word(sp + 0x10) & 0xFFFFFF;
        int px = (int)cpu->read_word(sp + 0), py = (int)cpu->read_word(sp + 4), pz = (int)cpu->read_word(sp + 8);
        /* centre RTPS: swizzle (camera-relative >>2), TR=0, camera matrix */
        double VXg = (double)(camY - py) / 4.0;      /* GTE-X (÷4 fractional, not >>2) */
        double VYg = (double)(camZ - pz) / 4.0;      /* GTE-Y */
        double VZg = (double)(px - camX) / 4.0;      /* GTE-Z */
        projres c; proj2(R, ofx, ofy, v, VXg, VYg, VZg, &c);
        if (c.m3 <= 0.0) continue;

        /* SCALE: fade8 = |m_Speed − newLife|·256/m_Speed -> scaledFade pulses the
         * twinkle. ANGLE: indexed by the animated m_0x0E seed (+0xE), which the
         * .s increments ~3·dt/frame for a smooth spin (NOT fade8 - that only
         * scales). Read post-.s m_0x0E (the value the .s used + wrote). */
        int angIdx = (int)((w3 >> 16) & 0xFF);       /* m_0x0E (spin angle) */
        /* Sub-tick phase: lerp life + spin between this slot's prev/curr
         * interp-objs snapshots (see s_phase_now above). Guards: same slot
         * identity (position + m_Speed unchanged) and life non-increasing -
         * a respawn re-arms m_Life upward -> snap, never lerp across it. */
        double lifef = (double)newLife, angf = (double)angIdx;
        int phase_interp = 0;
        if (s_phase_now) {
            const openpete_interp_obj_t* o = openpete_interp_objs_find(sp);
            if (o && o->have_prev) {
                /* Endpoints + identity guards from the TABLE pair (prev AND
                 * curr) - live RAM is the committed tick under runahead
                 * (≈ table-prev): lerping prev->live degenerated the twinkle
                 * back to 30 Hz, and prev-vs-LIVE-pos identity would
                 * false-snap at N>=2. */
                uint32_t pw3, pp0, pp1, pp2, cw3, cp0, cp1, cp2;
                memcpy(&pw3, o->prev + 0x0C, 4);
                memcpy(&pp0, o->prev + 0x00, 4);
                memcpy(&pp1, o->prev + 0x04, 4);
                memcpy(&pp2, o->prev + 0x08, 4);
                memcpy(&cw3, o->curr + 0x0C, 4);
                memcpy(&cp0, o->curr + 0x00, 4);
                memcpy(&cp1, o->curr + 0x04, 4);
                memcpy(&cp2, o->curr + 0x08, 4);
                int pLife = (int)(pw3 & 0xFF), pSpeed = (int)((pw3 >> 8) & 0xFF);
                int pAng  = (int)((pw3 >> 16) & 0xFF);
                int cLife = (int)(cw3 & 0xFF), cSpeed = (int)((cw3 >> 8) & 0xFF);
                int cAng  = (int)((cw3 >> 16) & 0xFF);
                double a;
                if (pLife >= cLife && cLife > 0 && pSpeed == cSpeed &&
                    pp0 == cp0 && pp1 == cp1 && pp2 == cp2 && part_slot_alpha(o, &a)) {
                    lifef = (double)pLife + ((double)cLife - (double)pLife) * a;
                    angf = lerp_wrap_b(pAng, cAng, a);
                    phase_interp = 1;
                }
            }
        }
        /* OPENPETE_SPARKLE_INTERP_DBG: per-present phase trace for slot 0 -
         * fractional lifef/angf between the byte grid prove the sub-tick sweep. */
        {
            static int s_sdbg = -1;
            if (s_sdbg < 0) { const char* e = getenv("OPENPETE_SPARKLE_INTERP_DBG"); s_sdbg = (e && *e && *e != '0'); }
            if (s_sdbg && i == 0)
                fprintf(stderr, "[sparkle-phase f=%u] slot0 %s life=%.3f ang=%.3f (raw %d/%d)\n",
                        openpete_frame, phase_interp ? "lerp" : "snap", lifef, angf, newLife, angIdx);
        }
        double scaledFade, sinv, cosv;
        if (phase_interp) {
            /* Float mirror of the integer math below (identical at alpha=1):
             * fractional fade8 + sin/cos lerped between adjacent table entries
             * (exact at integer angles), so the pulse/spin sweep sub-tick. */
            double dff = (double)speed - lifef; if (dff < 0.0) dff = -dff;
            double fade8f = dff * 256.0 / (double)speed;
            if (fade8f < 0.0) fade8f = 0.0; else if (fade8f > 255.0) fade8f = 255.0;
            scaledFade = (256.0 - fade8f) * (double)fade;
            ngte_sincos_f(cpu, angf, &sinv, &cosv);
        } else {
            /* canonical integer path - byte-exact for the A/B harness */
            int df = speed - newLife; if (df < 0) df = -df;
            int fade8 = speed ? (df * 256) / speed : 0;
            if (fade8 < 0) fade8 = 0; else if (fade8 > 255) fade8 = 255;
            scaledFade = (double)(256 - fade8) * fade;
            sinv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx*2);
            cosv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx*2 + 128);  /* cos = +0x80 */
        }
        double ec = scaledFade * cosv / 524288.0;    /* >>19 */
        double es = scaledFade * sinv / 524288.0;
        /* tips view-space (VZ=0); transform diag(a3)+TR=2*centre, then project */
        double a3 = c.m3 > 4096.0 ? c.m3 : 4096.0;
        double TR0 = c.m1*2.0, TR1 = c.m2*2.0, TR2 = c.m3*2.0;
        double sz3 = TR2 < 0 ? 0 : (TR2 > 65535 ? 65535 : TR2);
        double qd = part_cap(sz3, Hc, H) ? qcap : (double)H/sz3;
        int styleByte = (int)(cpu->read_word(sp + 0x10) >> 24);  /* pad byte +0x13 */
        int style = styleByte & 0x0F;
        int hue = (styleByte >> 4) & 0x0F;                   /* 1..6 = k_s3_glint row+1 */
        if (hue > 6) hue = 6;
        /* S3 two-mode switch: a hued glint beyond the near threshold is
         * drawn as the plain cross. S3's far path colours its lines
         * with word0 (asm: t2+0x06000000 at 0x8002F4D8), but that pale
         * tint on a 1px PSX line READS white - on our crisp native
         * lines it reads salmon/green/etc. Perceptual fidelity wins: far cross stays stock white. */
        if (style == 1 && hue > 0 && c.m3 >= S3_GLINT_NEAR_M3)
            style = 0;
        if (style == 1 && s_sstar_n < MAX_SSTAR) {
            /* 4-point star flare: tips = the stock cross's 4 diagonal
             * endpoints (same pulse+spin), waist points 45° between at
             * ~0.21R - a filled concave star instead of two lines. */
            double wc = (ec - es) * 0.2121, ws = (es + ec) * 0.2121;
            const double P[8][2] = {
                { ec,  es}, { wc,  ws}, {-es,  ec}, {-ws,  wc},
                {-ec, -es}, {-wc, -ws}, { es, -ec}, { ws, -wc} };
            sstar* st = &s_sstar[s_sstar_n++];
            st->sx[0] = (float)(ofx + clamp16(TR0)*qd);   /* centre */
            st->sy[0] = (float)(ofy + clamp16(TR1)*qd);
            double px[8], py[8];
            affine_project(P, 8, TR0, TR1, a3 / 4096.0, a3 / 4096.0, qd, ofx, ofy, px, py);
            for (int k = 0; k < 8; k++) { st->sx[k+1] = (float)px[k]; st->sy[k+1] = (float)py[k]; }
            st->cz = (float)c.m3 * PART_VZ_WORLD4;
            st->rgb = color;
            st->hue = hue;
            continue;
        }
        const double T[4][2] = { {-ec,-es}, {es,-ec}, {-es,ec}, {ec,es} };
        double sx[4], sy[4];
        affine_project(T, 4, TR0, TR1, a3 / 4096.0, a3 / 4096.0, qd, ofx, ofy, sx, sy);
        /* line 1 = T0->T3, line 2 = T1->T2 */
        sline* l0 = &s_sline[s_sline_n++];
        l0->ax=(float)sx[0]; l0->ay=(float)sy[0]; l0->bx=(float)sx[3]; l0->by=(float)sy[3];
        l0->cz=(float)c.m3 * PART_VZ_WORLD4; l0->rgb=color;   /* world/4 -> full-world */
        sline* l1 = &s_sline[s_sline_n++];
        l1->ax=(float)sx[1]; l1->ay=(float)sy[1]; l1->bx=(float)sx[2]; l1->by=(float)sy[2];
        l1->cz=(float)c.m3 * PART_VZ_WORLD4; l1->rgb=color;
        if (streak_dbg()) { streak_log("sparkle", sp, sx[0], sy[0], sx[3], sy[3]);
                            streak_log("sparkle", sp, sx[1], sy[1], sx[2], sy[2]); }
    }
}

/* OT source-body pull. The PSX links a moby's whole model into ONE OT
 * bucket at its centre depth and the sparkle renderer appends into that
 * same bucket AFTER the mobys (tail-append idiom at 0x80058508 ff.), so a
 * sparkle draws over its ENTIRE source moby; per-pixel Z inverts that tie
 * (the body's front polys are nearer than the centre-spawned sparkle).
 * Pull sparkle depth by a fixed distance that clears typical source
 * bodies (gems ~250, chests ~300, statue parts more). This is a
 * deliberate carve-out from the true-stored-depth rule: the og behaviour
 * is same-bucket APPEND ORDER - the sparkle beats its source regardless
 * of body size - which no sub-slot tie pull can express; the pull must
 * span the source body, not one OT slot. Residual vs PSX: geometry
 * BETWEEN sparkle and camera within this margin is overdrawn (PSX's own
 * pierce bound was its ~128-unit bucket); transient 5px glints make that
 * invisible in practice. */
#define SPARKLE_OT_PULL 1024.0f

/** @brief Builds the sparkles and feeds them into the native scene.
 *
 * The scene holds triangles only. Each style-1 star becomes an 8-triangle
 * additive fan from a core to the alternating tip and waist outline points:
 * the core is the S3 glint colour word 0 or, for a plain star, m_Color
 * doubled and clamped, and the rim is the hue's word 1 and word 2 or
 * m_Color. Each cross line becomes ngte_line_quad's opaque quad in
 * m_Color. Depth is the view-Z pulled forward by SPARKLE_OT_PULL.
 * Display-only: the .s already ran and wrote every verdict byte.
 * @param cpu The guest CPU state. */
static void sparkle_feed_native(CPUState* cpu) OP_REQUIRES(sim) {
    sparkle_build(cpu);
    if (s_sline_n <= 0 && s_sstar_n <= 0) return;
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(
        OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_FLARE, 0, 0));
    for (int i = 0; i < s_sstar_n; i++) {
        sstar* st = &s_sstar[i];
        float depth = openpete_native_scene_viewz(
            openpete_worldvz_pull(openpete_worldvz_full(st->cz), SPARKLE_OT_PULL));
        uint32_t core, tipc, waistc;
        if (st->hue > 0) {
            /* S3 near-star gouraud (func_8002F098): centre = word0
             * (pale, white-hot under additive), spike tips = word1
             * (deep hue), waists = word2 (S3's near-black ring). The
             * hue IS the gradient - additive falloff along each arm. */
            core   = k_s3_glint[st->hue - 1][0];
            tipc   = k_s3_glint[st->hue - 1][1];
            waistc = k_s3_glint[st->hue - 1][2];
        } else {
            /* legacy: rim = m_Color, core pushed toward white (x2) */
            uint32_t rim = st->rgb & 0xFFFFFFu;
            uint32_t cr = ((rim >> 0) & 0xFF) * 2; if (cr > 255) cr = 255;
            uint32_t cg = ((rim >> 8) & 0xFF) * 2; if (cg > 255) cg = 255;
            uint32_t cb = ((rim >> 16) & 0xFF) * 2; if (cb > 255) cb = 255;
            core = cr | (cg << 8) | (cb << 16);
            tipc = waistc = rim;
        }
        for (int k = 0; k < 8; k++) {
            int k2 = (k + 1) & 7;
            openpete_scene_tri t = {0};
            t.v[0].sx = st->sx[0];    t.v[0].sy = st->sy[0];
            t.v[1].sx = st->sx[k+1];  t.v[1].sy = st->sy[k+1];
            t.v[2].sx = st->sx[k2+1]; t.v[2].sy = st->sy[k2+1];
            /* outline points alternate tip (even k) / waist (odd k) */
            uint32_t oc1 = (k  & 1) ? waistc : tipc;
            uint32_t oc2 = (k2 & 1) ? waistc : tipc;
            for (int m = 0; m < 3; m++) {
                uint32_t c = m == 0 ? core : (m == 1 ? oc1 : oc2);
                t.v[m].z = depth; t.v[m].pvz = depth; t.v[m].u = 0.0f; t.v[m].v = 0.0f;
                t.v[m].rgba = (c & 0xFFFFFFu) | 0xFF000000u;
            }
            t.textured = 0;
            t.tpage_x = t.tpage_y = t.clut_x = t.clut_y = t.bpp = 0;
            t.blend = 2;                          /* additive flare */
            openpete_native_scene_add_tri(&t, OPENPETE_SCENE_WORLD, 0);
        }
    }
    openpete_native_scene_set_tag(0);
    if (s_sline_n <= 0) return;
    openpete_native_scene_set_tag(
        OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_SPARK, 0, 0));
    for (int i = 0; i < s_sline_n; i++) {
        const sline* l = &s_sline[i];
        const float depth = openpete_native_scene_viewz(
            openpete_worldvz_pull(openpete_worldvz_full(l->cz), SPARKLE_OT_PULL));
        part_feed_line_quad(l->ax, l->ay, l->bx, l->by, l->rgb, l->rgb, depth,
                            0, OPENPETE_SCENE_WORLD);   /* LINE_F 0x40 = opaque */
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Compares the native sparkle lines against the renderer's captured LINE_F prims (code 0x40).
 *
 * Rebuilds the sparkles, then matches every native line endpoint to its
 * nearest .s line endpoint (raw[2], raw[3], packed (Y << 16) | X) by
 * Manhattan distance. Prints one [sparkle-ab] line with the line and prim
 * counts, the mean and the worst distance.
 * @param cpu The guest CPU state. */
static void sparkle_compare(CPUState* cpu) {
    sparkle_build(cpu);
    int prim_line = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x40) prim_line++;
    double sum = 0, worst = 0; int nn = 0;
    for (int t = 0; t < s_sline_n; t++) {
        double e[2][2] = { {s_sline[t].ax, s_sline[t].ay}, {s_sline[t].bx, s_sline[t].by} };
        for (int k = 0; k < 2; k++) {
            double best = 1e9;
            for (int p = 0; p < s_prim_n; p++) {
                if (s_prim[p].code != 0x40) continue;
                for (int m = 2; m <= 3; m++) {       /* raw[2]=v0, raw[3]=v1 packed (Y<<16)|X */
                    double lx = (double)(int16_t)(s_prim[p].raw[m] & 0xFFFF);
                    double ly = (double)(int16_t)(s_prim[p].raw[m] >> 16);
                    double d = (e[k][0]-lx<0?lx-e[k][0]:e[k][0]-lx) + (e[k][1]-ly<0?ly-e[k][1]:e[k][1]-ly);
                    if (d < best) best = d;
                }
            }
            if (best < 1e8) { sum += best; if (best > worst) worst = best; nn++; }
        }
    }
    fprintf(stderr, "[sparkle-ab f=%u] native_line=%d prim_line=%d nn=%.2f worst=%.1fpx\n",
            openpete_frame, s_sline_n, prim_line, nn?sum/nn:0, worst);
}

/* ---- dragon-rescue star flash (func_80058864) - float walk + feed + A/B ---- *
 * The "free a dragon" 8-point star sparkle (e.g. Nestor in Artisans). Unlike the
 * particle/glow/sparkle pools this is a SINGLE gamestate record D_80076248,
 * decoded from the recompiled C (block_80058864) cross-checked against the
 * captured prims:
 *   +0x00 gate (active flag; skip when 0)   +0x04 Vector3D world position
 *   +0x10 SHORTMATRIX (per-effect rotation; built each frame by
 *         RotVec8ToMatrix(+0x28 rot8,mat,g_Camera.projMat) so it already folds
 *         in the camera projection -> RTPS through it maps local spoke -> view
 *         space; dragonstar_matrix recomposes it in double from the angles)
 *   +0x24 spoke size      +0x2B centre colour byte (broadcast to (c,c,c))
 * The .s sets TR = view-space centre = camR·(camY-py,camZ-pz,px-camX) (the same
 * swizzle particles/glows use), projects the centre (V=0), then RTPS-projects 16
 * ring spokes through `matrix`+TR. Ring radius alternates: even points
 * (sin/cos·size)>>12, odd points >>10 (4x farther) -> an 8-point star. Each PSX
 * prim is a FLAT tri (code 0x22 = F3+STP) of one colour (c,c,c); a separate
 * 0xE1000620 draw-mode prim makes the whole effect ABR mode 1 ADDITIVE. Output
 * OT = g_HudOT (over the world). Display-only & determinism-safe: the .s writes
 * no gameplay-readable byte (the producer func_8002F3E4 animates D_80076248). */

static gtri s_dstri[16]; static int s_dstri_n;

/* Sub-tick euler override for the star's +0x28 angles, {Z, X, Y} byte angles
 * at float precision; s_ds_rot_on 0 reads the record's own bytes. */
static double s_ds_rot[3];
static int    s_ds_rot_on;

void dragonstar_native_set_rot_interp(const double* ang_zxy) {
    s_ds_rot_on = (ang_zxy != NULL);
    if (ang_zxy) memcpy(s_ds_rot, ang_zxy, sizeof s_ds_rot);
}

/** @brief The star's spoke matrix as gamestate 8 builds it before func_80058864, composed in double.
 *
 * The game builds it with RotVec8ToMatrix(+0x28 angles,
 * g_Camera.m_ProjectionMatrix) into +0x10 (func_8001CFDC); here it is composed
 * in double from the camera matrix the view resolves (the bracketed camera on a
 * sub-tick present) and the angles, which are the record's bytes on a canonical
 * present and the orchestrator's float lerp on a sub-tick one. The record's
 * +0x10 holds the GTE's truncated copy of the same product and is not read.
 * @param cpu guest CPU state
 * @param v   the resolved view; its cam_M is the left factor
 * @param Me  receives the spoke matrix, Q12 scale, unrounded */
static void dragonstar_matrix(CPUState* cpu, const ngte_view* v, double Me[3][3]) {
    double ang[3];
    if (s_ds_rot_on) {
        memcpy(ang, s_ds_rot, sizeof ang);
    } else {
        uint32_t w = cpu->read_word(D_DRAGONSTAR + 0x28);
        ang[0] = (double)(w & 0xFFu);
        ang[1] = (double)((w >> 8) & 0xFFu);
        ang[2] = (double)((w >> 16) & 0xFFu);
    }
    ngte_compose_euler_f(cpu, v->cam_M, ang, Me);
}

/** @brief Builds the star's 16-triangle centre fan into s_dstri.
 *
 * Projects the record's centre, then the 16 ring spokes (even points at size,
 * odd points at 4x size) through the spoke matrix and the centre's view
 * translation. Leaves s_dstri_n 0 when the record's gate is clear or the centre
 * is behind the camera.
 * @param cpu guest CPU state */
static void dragonstar_build(CPUState* cpu) {
    s_dstri_n = 0;
    if (cpu->read_word(D_DRAGONSTAR + 0x00) == 0) return;       /* inactive */

    ngte_view_build(cpu, &s_pv);
    const ngte_view* v = &s_pv;
    int Rc[3][3]; memcpy(Rc, v->cam_M, sizeof Rc);
    double camX = v->cam[0], camY = v->cam[1], camZ = v->cam[2];
    double ofx = v->ofx, ofy = v->ofy;
    double H   = v->H_eff;
    const double Hc = v->H_canon, qcap = NGTE_DIV_CAP * v->s;  /* GTE limit regime on the GTE's H; capped quotient scales with the view */

    int px = (int)cpu->read_word(D_DRAGONSTAR+0x04);
    int py = (int)cpu->read_word(D_DRAGONSTAR+0x08);
    int pz = (int)cpu->read_word(D_DRAGONSTAR+0x0C);

    /* TR = view-space centre (camera matrix · swizzle); project it (V=0). */
    projres c; proj2(Rc, ofx, ofy, v, (double)(camY-py), (double)(camZ-pz), (double)(px-camX), &c);
    double trx = c.m1, try_ = c.m2, trz = c.m3;
    if (trz <= 0.0) return;                                     /* behind camera */

    double Me[3][3]; dragonstar_matrix(cpu, v, Me);             /* per-effect rotation */
    int size = (int)cpu->read_word(D_DRAGONSTAR+0x24);
    uint32_t cc = cpu->read_word(D_DRAGONSTAR+0x28) >> 24;      /* +0x2B colour byte */
    cc &= 0xFF;
    uint32_t color = cc | (cc<<8) | (cc<<16);

    /* 16 ring points in angular order: idx = m*16; even m -> inner (>>12), odd m
     * -> outer (>>10). spoke local = ((sin·size)>>shift, 0, (cos·size)>>shift),
     * RTPS through the per-effect matrix + TR. COSINE_8 = SINE_8 + 0x80. */
    double rx[16], ry[16];
    for (int m = 0; m < 16; m++) {
        int idx   = m * 16;
        int sinv  = rd_s16(cpu, D_SIN_TABLE        + (uint32_t)idx*2);
        int cosv  = rd_s16(cpu, D_SIN_TABLE + 0x80 + (uint32_t)idx*2);
        /* spoke radius: even points >>12, odd >>10 - kept as FLOAT division (not
         * the .s integer arithmetic shift) so the spoke length carries sub-pixel
         * precision. The native renderer's whole point is no positional
         * quantization; the int floor is the .s artefact we improve on. */
        double rscale = (m & 1) ? 1024.0 : 4096.0;
        double vx = (double)sinv * (double)size / rscale;
        double vz = (double)cosv * (double)size / rscale;
        double mac1 = (Me[0][0]*vx + Me[0][2]*vz)/4096.0 + trx;
        double mac2 = (Me[1][0]*vx + Me[1][2]*vz)/4096.0 + try_;
        double mac3 = (Me[2][0]*vx + Me[2][2]*vz)/4096.0 + trz;
        double sz3 = mac3 < 0.0 ? 0.0 : (mac3 > 65535.0 ? 65535.0 : mac3);
        double q = part_cap(sz3, Hc, H) ? qcap : (H / sz3);
        rx[m] = ofx + clamp16(mac1) * q;
        ry[m] = ofy + clamp16(mac2) * q;
    }

    for (int m = 0; m < 16; m++) {                              /* centre fan */
        int n = (m + 1) & 15;
        gtri* t = &s_dstri[s_dstri_n++];
        t->sx[0]=(float)c.sx;  t->sy[0]=(float)c.sy;  t->rgb[0]=color;   /* centre */
        t->sx[1]=(float)rx[m]; t->sy[1]=(float)ry[m]; t->rgb[1]=color;
        t->sx[2]=(float)rx[n]; t->sy[2]=(float)ry[n]; t->rgb[2]=color;
        t->cz = (float)trz;
    }
}

/** @brief Builds the dragon-rescue star fan and feeds it into the native scene.
 *
 * Emits untextured flat triangles in the additive blend group (blend 2) at
 * view-Z 1, the near plane: the additive pipeline writes no depth, so the
 * star draws over the whole 3D scene as the PSX's g_HudOT link does. Emits
 * nothing when the record's gate is clear. Display-only.
 * @param cpu The guest CPU state. */
static void dragonstar_feed_native(CPUState* cpu) OP_REQUIRES(sim) {
    dragonstar_build(cpu);
    if (s_dstri_n <= 0) return;
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);
    /* g_HudOT overlay: the PSX links this into the HUD OT, drawn OVER the whole 3D
     * scene. blend=2 puts it in the native additive group (drawn last, over the
     * player); view-Z 1 (the near plane) so the depth test never rejects a
     * spike that crosses nearer ground/Spyro - the additive pipe is
     * depth_write_off, so this is an on-top additive overlay, mirroring the PSX
     * HUD-OT order. One depth for the whole overlay: W is constant. */
    float depth = 1.0f;
    openpete_native_scene_set_tag(
        OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_STAR, 0, 0));
    for (int i = 0; i < s_dstri_n; i++) {
        gtri* t = &s_dstri[i];
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = t->sx[k]; st.v[k].sy = t->sy[k]; st.v[k].z = depth; st.v[k].pvz = depth;
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (t->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 2;                                  /* ABR mode 1 (additive) */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Compares the native star fan against the renderer's captured F3 semi-transparent prims (code 0x22).
 *
 * Rebuilds the fan, then matches every native vertex to its nearest .s F3
 * vertex by Manhattan distance, independent of order. Prints one [dstar-ab]
 * line with the triangle and prim counts, the mean and the worst distance.
 * @param cpu The guest CPU state. */
static void dragonstar_compare(CPUState* cpu) {
    dragonstar_build(cpu);
    int prim22 = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x22) prim22++;
    double sum = 0, worst = 0; int nn = 0;
    for (int t = 0; t < s_dstri_n; t++) for (int k = 0; k < 3; k++) {
        double ax = s_dstri[t].sx[k], ay = s_dstri[t].sy[k], best = 1e9;
        for (int p = 0; p < s_prim_n; p++) {
            if (s_prim[p].code != 0x22) continue;
            for (int mv = 0; mv < 3; mv++) {
                double dx = ax - s_prim[p].x[mv], dy = ay - s_prim[p].y[mv];
                double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
                if (d < best) best = d;
            }
        }
        if (best < 1e8) { sum += best; if (best > worst) worst = best; nn++; }
    }
    fprintf(stderr, "[dstar-ab f=%u] native_tri=%d prim22=%d nn=%.2f worst=%.1fpx\n",
            openpete_frame, s_dstri_n, prim22, nn?sum/nn:0, worst);
}

/* ---- gating --------------------------------------------------------------- */
/** @brief Reports whether OPENPETE_PARTICLE_VERIFY enables the particle verify mode.
 *
 * Verify mode captures the renderers' prims, dumps the pools and prims once
 * and runs the native-versus-.s A/B compares. The variable is read once and
 * cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int verify_enabled(void) {
    OP_ROLE_ASSERT(sim);
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PARTICLE_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
/** @brief Returns the openpete_frame at which the verify dumps and compares start.
 *
 * Parsed from OPENPETE_PARTICLE_VERIFY_FRAME with strtoul base 0, 0 when
 * unset. The variable is read once and cached.
 * @return The first openpete_frame value the verify output prints at. */
static unsigned verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_PARTICLE_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

/* Per-tick path census (OPENPETE_PARTICLE_PATH_CENSUS=1). The three pools
 * outlive their renderers: a pool can hold active records on a tick where the
 * game never called that renderer, or called it and it emitted nothing. Under
 * the retired shared-path gating any such tick replayed the pool on every
 * intermediate present whenever any sibling renderer had drawn; the dragon
 * star still shares RP_PARTICLES, so its exposure is the live number. One
 * record per tick (openpete_frame at the hook), evaluated when the next tick's
 * first hook arrives; a totals line at exit for the corpus harness. */
typedef struct {
    unsigned frame; int ran_p, ran_g, ran_s, ran_star;
    uint32_t bytes_p, bytes_g, bytes_s, bytes_star;
    int pool_glow, pool_spark, pool_star;
} pcen_tick_t;
static pcen_tick_t s_pcen; static int s_pcen_on = -1;
static uint32_t s_pcen_ticks, s_pcen_active[3], s_pcen_ran[3], s_pcen_exposed[3], s_pcen_first[3];
/* Sparkle walk vs .s on the canonical hook: twinkles the native walk emits
 * beyond what the .s drew (extra) and short of it (missing), per tick. */
static uint32_t s_pcen_sp_ticks, s_pcen_sp_extra_ticks, s_pcen_sp_extra, s_pcen_sp_missing_ticks, s_pcen_sp_missing, s_pcen_sp_extra_first;
/** @brief Prints the path census totals to stderr at exit.
 *
 * Registered with atexit by particle_path_census_note. Prints the tick
 * count, the glow, sparkle and star active, ran, exposed and first-exposed
 * tick counts, and the sparkle-walk extra and missing totals. */
static void particle_path_census_exit(void) {
    OP_ROLE_ASSERT(sim);
    fprintf(stderr, "particle path census: ticks=%u glow: active=%u ran=%u exposed=%u first=%u | "
            "sparkle: active=%u ran=%u exposed=%u first=%u | star: active=%u ran=%u exposed=%u first=%u | "
            "sparkle-walk: ticks=%u extra_ticks=%u extra=%u missing_ticks=%u missing=%u extra_first=%u\n",
            s_pcen_ticks, s_pcen_active[0], s_pcen_ran[0], s_pcen_exposed[0], s_pcen_first[0],
            s_pcen_active[1], s_pcen_ran[1], s_pcen_exposed[1], s_pcen_first[1],
            s_pcen_active[2], s_pcen_ran[2], s_pcen_exposed[2], s_pcen_first[2],
            s_pcen_sp_ticks, s_pcen_sp_extra_ticks, s_pcen_sp_extra, s_pcen_sp_missing_ticks, s_pcen_sp_missing, s_pcen_sp_extra_first);
}
static void sparkle_build(CPUState* cpu);
/** @brief Counts, for one sparkle hook call, the twinkles the native walk builds beyond and short of the .s.
 *
 * Runs after the .s drew and its prims were captured. Rebuilds the
 * sparkles and compares the native cross count (s_sline_n / 2; stars are
 * not counted) with the captured LINE_F (0x40) prims / 2, accumulating the
 * extra and missing totals, their tick counts and the first extra tick.
 * No-op unless OPENPETE_PARTICLE_PATH_CENSUS is on.
 * @param cpu The guest CPU state.
 * @param captured 1 when s_prim holds this call's prims, else 0. */
static void particle_path_census_sparkle(CPUState* cpu, int captured) {
    if (s_pcen_on != 1) return;
    sparkle_build(cpu);
    int native = s_sline_n / 2, prims = 0;
    if (captured) for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x40) prims++;
    prims /= 2;
    s_pcen_sp_ticks++;
    if (native > prims) { if (s_pcen_sp_extra_ticks++ == 0) s_pcen_sp_extra_first = openpete_frame; s_pcen_sp_extra += (uint32_t)(native - prims); }
    if (native < prims) { s_pcen_sp_missing_ticks++; s_pcen_sp_missing += (uint32_t)(prims - native); }
}
/** @brief Closes the census record of the previous tick into the totals.
 *
 * Does nothing before the first record. Counts the tick, then per pool
 * (glow, sparkle, star) counts it active when the pool held records and ran
 * when its renderer was called. It counts the pool exposed, and records the
 * first exposed tick, when the pool was active, its renderer drew no bytes
 * and its gate drew: any renderer for glow and sparkle, the particle
 * renderer or the star for the star. */
static void particle_path_census_flush(void) {
    pcen_tick_t* t = &s_pcen;
    if (t->frame == 0) return;
    s_pcen_ticks++;
    /* Retired gating: RP_PARTICLES drawn by any sibling; current star gating: the
     * path the star shares (particles or the star itself) drawn. */
    const int any_drawn  = t->bytes_p > 0 || t->bytes_g > 0 || t->bytes_s > 0 || t->bytes_star > 0;
    const int part_drawn = t->bytes_p > 0 || t->bytes_star > 0;
    const int active[3] = { t->pool_glow, t->pool_spark, t->pool_star };
    const int drew[3]   = { t->ran_g && t->bytes_g > 0, t->ran_s && t->bytes_s > 0, t->ran_star && t->bytes_star > 0 };
    const int ran[3]    = { t->ran_g, t->ran_s, t->ran_star };
    const int gate[3]   = { any_drawn, any_drawn, part_drawn };
    for (int k = 0; k < 3; k++) {
        if (active[k]) s_pcen_active[k]++;
        if (ran[k]) s_pcen_ran[k]++;
        if (active[k] && !drew[k] && gate[k]) {
            if (s_pcen_exposed[k]++ == 0) s_pcen_first[k] = t->frame;
        }
    }
}
/** @brief Records one renderer call in the current tick's census record.
 *
 * On first use reads OPENPETE_PARTICLE_PATH_CENSUS and, when on, registers
 * the exit print. When openpete_frame differs from the record's, flushes
 * the previous record, clears it and samples the pools: any glow slot with
 * a non-zero first word, any sparkle with a non-zero life byte, and the
 * D_DRAGONSTAR gate word. Then counts the call and its polybuf bytes under
 * the renderer.
 * @param cpu The guest CPU state.
 * @param tag 'p' particles, 'g' glows, 's' sparkles, any other value the
 *     star.
 * @param bytes The polybuf bytes the renderer emitted. */
static void particle_path_census_note(CPUState* cpu, char tag, uint32_t bytes) {
    if (s_pcen_on < 0) {
        const char* e = getenv("OPENPETE_PARTICLE_PATH_CENSUS");
        s_pcen_on = (e && *e && *e != '0') ? 1 : 0;
        if (s_pcen_on) atexit(particle_path_census_exit);
    }
    if (!s_pcen_on) return;
    if (s_pcen.frame != openpete_frame) {
        particle_path_census_flush();
        memset(&s_pcen, 0, sizeof s_pcen);
        s_pcen.frame = openpete_frame;
        for (int i = 0; i < 16 && !s_pcen.pool_glow; i++)
            if (cpu->read_word(D_GLOWS + (uint32_t)i*0x24) != 0) s_pcen.pool_glow = 1;
        for (int i = 0; i < 8 && !s_pcen.pool_spark; i++)
            if ((cpu->read_word(G_SPARKLES + (uint32_t)i*0x18 + 0xC) & 0xFF) != 0) s_pcen.pool_spark = 1;
        s_pcen.pool_star = cpu->read_word(D_DRAGONSTAR) != 0;
    }
    switch (tag) {
    case 'p': s_pcen.ran_p = 1; s_pcen.bytes_p += bytes; break;
    case 'g': s_pcen.ran_g = 1; s_pcen.bytes_g += bytes; break;
    case 's': s_pcen.ran_s = 1; s_pcen.bytes_s += bytes; break;
    default:  s_pcen.ran_star = 1; s_pcen.bytes_star += bytes; break;
    }
}

/* The three particle renderers share one latch: they are separate guest
 * entries, so the descriptor is built per call around the caller's own draw. */
static hook_bracket_state s_particle_hb;
/* ---- held camera cut: the particle, glow, sparkle and star records of tick N-1
 *
 * The g_Particles, glow, sparkle and dragon-star feeds re-derive their
 * primitives from their records, which hold tick N's state by the time a
 * sub-tick present runs. A held cut shows
 * tick N-1 under tick N-1's camera, so each renderer's hook copies its pool as
 * the renderer left it, the bytes the canonical feed read, two canonical epochs
 * deep, and the held present stages tick N-1's copy over the pool. */
#define GLOW_POOL_BYTES  (16u * 0x24u)
#define SPARK_POOL_BYTES (8u * 0x18u)
/* 256 records of 0x20 plus the terminator word the level loader stores at
 * record 256 (g_Particles[256], spyro-1 src/loaders.c). */
#define PART_POOL_BYTES  (256u * 0x20u + 4u)
/* The dragon-star record D_DRAGONSTAR, a pool of one record. */
#define STAR_REC_BYTES   0x2Cu

/* One pool's two copies: the current epoch's and the one it retired, each with
 * its live-record count and the guest address it was copied from. */
typedef struct {
    uint8_t* cur;
    uint8_t* old;
    int      n, n_old;
    uint32_t base, base_old;
} pool_copies_t;

static uint8_t       s_glow_pool[2][GLOW_POOL_BYTES];
static uint8_t       s_spark_pool[2][SPARK_POOL_BYTES];
static uint8_t       s_part_pool_copy[2][PART_POOL_BYTES];
static uint8_t       s_star_rec_copy[2][STAR_REC_BYTES];
static pool_copies_t s_glow_pc  = { s_glow_pool[0],  s_glow_pool[1],  0, 0, 0u, 0u };
static pool_copies_t s_spark_pc = { s_spark_pool[0], s_spark_pool[1], 0, 0, 0u, 0u };
static pool_copies_t s_part_pc  = { s_part_pool_copy[0], s_part_pool_copy[1], 0, 0, 0u, 0u };
static pool_copies_t s_star_pc  = { s_star_rec_copy[0],  s_star_rec_copy[1],  0, 0, 0u, 0u };

/** @brief Exchange a pool's current copy with the retired one, with their counts.
 *
 * @param pc The pool's copies. */
static void pool_copies_swap(pool_copies_t* pc) {
    uint8_t* t = pc->cur; pc->cur = pc->old; pc->old = t;
    const int n = pc->n; pc->n = pc->n_old; pc->n_old = n;
    const uint32_t b = pc->base; pc->base = pc->base_old; pc->base_old = b;
}
/** @brief Exchange the glow pool's two copies. */
static void glow_pool_swap(void)  { pool_copies_swap(&s_glow_pc); }
/** @brief Exchange the sparkle pool's two copies. */
static void spark_pool_swap(void) { pool_copies_swap(&s_spark_pc); }
/** @brief Exchange the g_Particles pool's two copies. */
static void part_pool_swap(void)  { pool_copies_swap(&s_part_pc); }
/** @brief Exchange the dragon-star record's two copies. */
static void star_rec_swap(void)   { pool_copies_swap(&s_star_pc); }

static cut_held_sets_t s_glow_held  = { glow_pool_swap,  &s_glow_pc.n,  "glow",      { 0u, 0u }, 0u };
static cut_held_sets_t s_spark_held = { spark_pool_swap, &s_spark_pc.n, "sparkle",   { 0u, 0u }, 0u };
static cut_held_sets_t s_part_held  = { part_pool_swap,  &s_part_pc.n,  "particles", { 0u, 0u }, 0u };
static cut_held_sets_t s_star_held  = { star_rec_swap,   &s_star_pc.n,  "star",      { 0u, 0u }, 0u };

/* A pool record's liveness: 1 live, 0 dead, -1 the pool's end (no later record
 * is read). */
typedef int (*pool_live_fn)(const uint8_t* rec);

int particle_glow_owner_mobys(uint32_t out[16]) {
    int n = 0;
    for (uint32_t i = 0; i < 16u; ++i) {
        const uint8_t* g = (const uint8_t*)openpete_psx_to_host_span(D_GLOWS + i * 0x24u, 0x24u);
        if (!g) break;
        int32_t cnt; uint32_t mpp;
        memcpy(&cnt, g + 0x00, 4);
        memcpy(&mpp, g + 0x08, 4);
        if (cnt <= 0 || mpp < 0x80000000u + 0x0Cu) continue;
        const uint32_t m = mpp - 0x0Cu;                      /* Moby.m_Position is +0x0C */
        const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_span(m, 0x58u);
        if (!mh || mh[0x48] >= 0x80u) continue;              /* unmapped or dead (m_State) */
        uint16_t cls; memcpy(&cls, mh + 0x36, 2);            /* m_Class */
        const uint8_t* row = (const uint8_t*)openpete_psx_to_host_span(G_MODELS + 4u * cls, 4u);
        uint32_t model = 0; if (row) memcpy(&model, row, 4);
        if (cls >= 512u || model == 0u) continue;            /* not a moby with a model row */
        int dup = 0;
        for (int k = 0; k < n; ++k) if (out[k] == m) dup = 1;
        if (!dup) out[n++] = m;
    }
    return n;
}

/** @brief Whether a glow record is live: its ring count (+0x00) is positive.
 *
 * @param rec The record.
 * @return 1 live, else 0. */
static int glow_rec_live(const uint8_t* rec) {
    int32_t w; memcpy(&w, rec, 4);
    return w > 0;
}
/** @brief Whether a sparkle record is live: its m_Life byte (+0x0C) is nonzero.
 *
 * @param rec The record.
 * @return 1 live, else 0. */
static int spark_rec_live(const uint8_t* rec) { return rec[0x0C] != 0; }
/** @brief Whether the dragon-star record is live: its gate word (+0x00) is nonzero, as dragonstar_build reads it.
 *
 * @param rec The record.
 * @return 1 live, else 0. */
static int star_rec_live(const uint8_t* rec) {
    uint32_t w; memcpy(&w, rec, 4);
    return w != 0u;
}
/** @brief Whether a g_Particles record is live, by its type byte (+0x01) as particle_build reads it.
 *
 * @param rec The record.
 * @return -1 at the 0xFF terminator, 0 for an inactive slot (bit 7 set), else 1. */
static int part_rec_live(const uint8_t* rec) {
    if (rec[1] == 0xFFu) return -1;
    return (rec[1] & 0x80u) ? 0 : 1;
}

/** @brief Copy a pool as its renderer left it into the current epoch's copy, retiring the previous epoch's first.
 *
 * Copies len bytes and records the guest address; the live-record count is
 * the number of whole records the predicate reports live before the first
 * end mark.
 * @param h The pool's held sets.
 * @param pc The pool's copies.
 * @param base The pool's guest address.
 * @param len The pool's size in bytes.
 * @param stride The record size in bytes.
 * @param live The pool's record liveness predicate. */
static void pool_capture(cut_held_sets_t* h, pool_copies_t* pc, uint32_t base,
                         uint32_t len, uint32_t stride, pool_live_fn live) OP_REQUIRES(sim) {
    const uint8_t* src = (const uint8_t*)openpete_psx_to_host_span(base, len);
    if (!src) return;
    cut_held_sets_capture(h);
    memcpy(pc->cur, src, len);
    pc->base = base;
    pc->n = 0;
    for (uint32_t off = 0; off + stride <= len; off += stride) {
        const int v = live(pc->cur + off);
        if (v < 0) break;
        pc->n += v;
    }
}

/** @brief Whether a pool's re-derivation may feed this present, staging tick N-1's copy over the pool on a held cut.
 *
 * Outside a hold, hook_reextract_ready. On a hold, cut_held_sets_enter picks
 * tick N-1's copy, which is staged through the sub-tick view so the build
 * reads it in place of tick N's records; a refused staging, or a copy taken
 * from another guest address than base, feeds nothing.
 * @param h The pool's held sets.
 * @param pc The pool's copies.
 * @param path The pool's render path.
 * @param base The pool's guest address.
 * @param len The pool's size in bytes.
 * @param hold Nonzero on a held camera cut.
 * @param held Receives 1 when the feed shows a staged held copy, else 0.
 * @return 1 when the feed may run, else 0. */
static int pool_feed_ready(cut_held_sets_t* h, pool_copies_t* pc, render_path_t path,
                           uint32_t base, uint32_t len, int hold, int* held) {
    const int pick = cut_held_sets_enter(h, hold);
    int ok = hook_reextract_ready_held(path, pick);
    *held = pick >= 0;
    if (ok && pick >= 0) {
        uint8_t* st = pc->base == base ? (uint8_t*)openpete_subtick_view_stage(base, len) : NULL;
        if (st) memcpy(st, pc->cur, len);
        else ok = 0;
    }
    cut_held_sets_leave(h, pick);
    return ok;
}

static void (*s_particle_realfn)(CPUState*);
/** @brief Runs the guest particle renderer chosen by on_particle_renderer as the bracket's draw.
 *
 * @param cpu The guest CPU state.
 * @param addr The hooked address (unused). */
static void particle_run_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim); (void)addr; s_particle_realfn(cpu); }
/** @brief Stashes the g_Particles pool pointer before the guest particle renderer runs.
 *
 * g_Particles points at one pool the level loader sets per level and the
 * allocator (func_80053570, func_80053608) works in place; the address the
 * .s walks is read here, and the native walks and the held-cut copy use
 * s_part_pool.
 * @param cpu The guest CPU state.
 * @param feed Unused.
 * @param verify Unused. */
static void particle_pre(CPUState* cpu, int feed, int verify) {
    OP_ROLE_ASSERT(sim);
    (void)feed; (void)verify;
    s_part_pool = cpu->read_word(G_PARTICLES);
}

/** @brief Hooks one guest particle renderer: runs it once, captures its prims and feeds the native scene.
 *
 * Brackets realfn with hook_bracket_run on the shared latch, stashes the
 * pool pointer before the draw, and notes the call in the path census.
 * With the feed or verify gate on and bytes emitted, captures the polybuf
 * range into s_prim; with the feed on and the render path enabled, feeds the
 * pool's native geometry by tag. In verify mode it also traces the first 30
 * calls of each renderer, dumps once per renderer from verify_frame on (the
 * pools too when dump_pools_too is set), and runs the matching A/B compare
 * up to 2000 times.
 * @param cpu The guest CPU state.
 * @param realfn The guest renderer.
 * @param tag "particles", "glows" or "sparkles"; the first letter selects
 *     the feed, compare and census slot.
 * @param path The render path the drawn interval is recorded under.
 * @param dump_pools_too 1 to print the pools with the first dump.
 * @return 0 when the bracket declines and the ordinary dispatch must run the
 *         renderer, 1 after the renderer ran here. */
static int on_particle_renderer(CPUState* cpu, void (*realfn)(CPUState*), const char* tag,
                                render_path_t path, int dump_pools_too) OP_REQUIRES(sim) {
    const hook_bracket_desc desc = {
        .path = path, .prof_tag = "particle.srend",
        .run = particle_run_draw, .verify_enabled = verify_enabled,
        .pre = particle_pre, .st = &s_particle_hb,
    };
    s_particle_realfn = realfn;
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, 0u, &desc, &startp, &endp, &feed, &verify)) return 0;

    particle_path_census_note(cpu, tag[0], endp - startp);

    if (verify) {
        static int s_trace[3] = {0,0,0};
        int slot = (tag[0]=='p') ? 0 : (tag[0]=='g') ? 1 : 2;
        if (s_trace[slot] < 30 && endp != startp) {
            fprintf(stderr, "[%s-call f=%u] polybuf %u bytes\n", tag, openpete_frame, endp - startp);
            s_trace[slot]++;
        }
    }

    if ((feed || verify) && endp > startp) {
        s_prim_n = 0;
        capture_prims(cpu, startp, endp);
    }
    if (tag[0] == 's') particle_path_census_sparkle(cpu, (feed || verify) && endp > startp);

    /* feed the native geometry into the scene: FT4 particles (tag 'p'), glow
     * rings (tag 'g'), or sparkle twinkles (tag 's'). */
    if (feed && endp > startp && render_path_enabled(path)) {
        if (tag[0] == 'p')      particle_feed_native(cpu);
        else if (tag[0] == 'g') glow_feed_native(cpu);
        else if (tag[0] == 's') sparkle_feed_native(cpu);
    }

    if (verify && endp > startp && openpete_frame >= verify_frame()) {
        static int s_dumped[3] = {0,0,0};
        int slot = (tag[0]=='p') ? 0 : (tag[0]=='g') ? 1 : 2;
        if (!s_dumped[slot]) {
            if (dump_pools_too) dump_pools(cpu);
            dump_prims(cpu, tag, startp, endp);
            s_dumped[slot] = 1;
        }
        if (slot == 0) {                                 /* particles: FT4 A/B */
            static int s_reports = 0;
            if (s_reports < 2000) { particle_compare(cpu); s_reports++; }
        } else if (slot == 1) {                          /* glows: ring fan A/B */
            static int s_greports = 0;
            if (s_greports < 2000) { glow_compare(cpu); s_greports++; }
        } else if (slot == 2) {                          /* sparkles: line A/B */
            static int s_sreports = 0;
            if (s_sreports < 2000) { sparkle_compare(cpu); s_sreports++; }
        }
    }

    return hook_bracket_end(&desc);
}

/** @brief Dispatch hook for func_800573C8, the particle renderer.
 *
 * @param cpu The guest CPU state.
 * @param addr The hooked address (unused).
 * A taken call then copies the g_Particles pool for a held camera cut
 * (pool_capture).
 * @return The on_particle_renderer verdict. */
static int on_800573C8_particles(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    (void)addr;
    const int took = on_particle_renderer(cpu, func_800573C8, "particles", RP_PARTICLES, 1);
    if (took) pool_capture(&s_part_held, &s_part_pc, s_part_pool, PART_POOL_BYTES, 0x20u, part_rec_live);
    return took;
}
/** @brief Dispatch hook for func_800580F4, the glow renderer.
 *
 * A taken call then copies the glow pool for a held camera cut (pool_capture).
 * @param cpu The guest CPU state.
 * @param addr The hooked address (unused).
 * @return The on_particle_renderer verdict. */
static int on_800580F4_glows(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    (void)addr;
    const int took = on_particle_renderer(cpu, func_800580F4, "glows", RP_GLOW, 0);
    if (took) pool_capture(&s_glow_held, &s_glow_pc, D_GLOWS, GLOW_POOL_BYTES, 0x24u, glow_rec_live);
    return took;
}
/** @brief Dispatch hook for func_800584C4, the sparkle renderer.
 *
 * A taken call then copies the sparkle pool for a held camera cut (pool_capture).
 * @param cpu The guest CPU state.
 * @param addr The hooked address (unused).
 * @return The on_particle_renderer verdict. */
static int on_800584C4_sparkles(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    (void)addr;
    const int took = on_particle_renderer(cpu, func_800584C4, "sparkles", RP_SPARKLE, 0);
    if (took) pool_capture(&s_spark_held, &s_spark_pc, G_SPARKLES, SPARK_POOL_BYTES, 0x18u, spark_rec_live);
    return took;
}

/** @brief Runs the guest dragon-rescue star renderer (func_80058864) as the hook bracket's draw.
 *
 * The star flash is a single-record effect outside the particle pools, so
 * it has its own hook, on_80058864_dragonstar, without the path census. The
 * hook runs this draw (g_HudOT prims for the PsyCross path), captures its
 * F3 prims for the A/B, and feeds the float star. Display-only. The star
 * shares RP_PARTICLES with the pool feeders, and the sub-tick re-extract
 * replays it only while that path is canon-drawn, so the hook records its
 * polybuf interval and marks the path whenever the native feed emitted: on
 * a tick where only the star draws, nothing else marks it, and the star
 * would show on canonical presents and vanish on the intermediates.
 * @param cpu The guest CPU state.
 * @param addr The hooked address (unused). */
static void dragonstar_run_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim); (void)addr; func_80058864(cpu); }

static hook_bracket_state s_dragonstar_hb;
static const hook_bracket_desc k_dragonstar_bracket = {
    .path = RP_PARTICLES, .prof_tag = "particle.srend",
    .run = dragonstar_run_draw, .verify_enabled = verify_enabled,
    .st = &s_dragonstar_hb, .flags = HB_NO_RENDER_PATHS,
};

/** @brief Dispatch hook for func_80058864: runs the star renderer once, captures its prims and feeds the float star.
 *
 * Brackets dragonstar_run_draw without --render-paths isolation. Notes the
 * call in the path census as the star, captures the polybuf range when the
 * feed or verify gate is on, and after the feed marks RP_PARTICLES drawn
 * when the star emitted triangles, then copies the record for a held camera
 * cut (pool_capture). In verify mode dumps the prims once from verify_frame
 * on and runs dragonstar_compare up to 2000 times.
 * @param cpu The guest CPU state.
 * @param addr The hooked address.
 * @return 0 when the bracket declines, 1 after the renderer ran here. */
static int on_80058864_dragonstar(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_dragonstar_bracket, &startp, &endp, &feed, &verify))
        return 0;
    particle_path_census_note(cpu, '*', endp - startp);

    if ((feed || verify) && endp > startp) { s_prim_n = 0; capture_prims(cpu, startp, endp); }
    if (feed) {
        dragonstar_feed_native(cpu);
        if (s_dstri_n > 0) render_path_mark_drawn(RP_PARTICLES);
    }
    pool_capture(&s_star_held, &s_star_pc, D_DRAGONSTAR, STAR_REC_BYTES, STAR_REC_BYTES, star_rec_live);

    if (verify && endp > startp && openpete_frame >= verify_frame()) {
        static int s_dumped = 0;
        if (!s_dumped) { dump_prims(cpu, "dstar", startp, endp); s_dumped = 1; }
        static int s_reports = 0;
        if (s_reports < 2000) { dragonstar_compare(cpu); s_reports++; }
    }
    return hook_bracket_end(&k_dragonstar_bracket);
}

/* Sub-tick re-extract (high-FPS spine): reproduce particles, glows and
 * sparkles from the current g_Particles pool - extract-only (no render_fn, no
 * verdicts). Pool walks self-limit to live particles; Sparx + ambient sparkles
 * stay present on intermediates. */
void particle_reextract(CPUState* cpu, int hold) OP_REQUIRES(sim) {
    if (!openpete_native_scene_enabled()) return;
    /* Each feeder replays only when ITS .s renderer drew on the canonical
     * tick. The pools outlive the renderers: a level transition stops all
     * three (else the spine replays the previous level's pool over the
     * cyclorama), and a dragon-rescue cutscene keeps rendering particles while
     * the glow and sparkle renderers are never called, leaving Sparx's glow
     * active in its pool but absent from every canonical present. */
    int part_held = 0, glow_held = 0, spar_held = 0, star_held = 0;
    const int part = pool_feed_ready(&s_part_held, &s_part_pc, RP_PARTICLES,
                                     s_part_pool, PART_POOL_BYTES, hold, &part_held);
    const int glow = pool_feed_ready(&s_glow_held, &s_glow_pc, RP_GLOW,
                                     D_GLOWS, GLOW_POOL_BYTES, hold, &glow_held);
    const int spar = pool_feed_ready(&s_spark_held, &s_spark_pc, RP_SPARKLE,
                                     G_SPARKLES, SPARK_POOL_BYTES, hold, &spar_held);
    const int star = pool_feed_ready(&s_star_held, &s_star_pc, RP_PARTICLES,
                                     D_DRAGONSTAR, STAR_REC_BYTES, hold, &star_held);
    if (!part && !glow && !spar && !star) return;
    /* Channel-phase interp on for the re-extract feeds only (every present when
     * the sub-tick spine is active - canonical-lag leg + intermediates); reset
     * so the canonical inline hook feeds stay integer/byte-exact for the A/Bs.
     * Covers particle spin/size/extents + line far endpoints + sparkle
     * life/spin (see the s_phase_now block). */
    const op_time_t now = openpete_interp_present_now();
    /* A staged held copy is tick N-1's pool as that tick drew it: no phase lerp. */
    s_phase_now = part_held ? 0 : now;
    if (part) particle_feed_native(cpu);
    s_phase_now = glow_held ? 0 : now;
    if (glow) glow_feed_native(cpu);
    s_phase_now = spar_held ? 0 : now;
    if (spar) sparkle_feed_native(cpu);
    s_phase_now = 0;
    if (star) dragonstar_feed_native(cpu);   /* self-gates on the record's +0x00 gate word */
}

static const feature_dispatch_entry_t particle_dispatch[] = {
    { 0x800573C8u, on_800573C8_particles, 0u },
    { 0x800580F4u, on_800580F4_glows, 0u },
    { 0x800584C4u, on_800584C4_sparkles, 0u },
    { 0x80058864u, on_80058864_dragonstar, 0u },
    { 0u, NULL, 0u },
};

const feature_module_t particle_module = {
    .name           = "particle_native",
    .apply_options  = NULL,
    .dispatch_table = particle_dispatch,
};
