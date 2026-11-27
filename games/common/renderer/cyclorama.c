// games/common/renderer/cyclorama.c - the cyclorama/sky renderer
// re-derived from gamestate + its autonomous numerical A/B harness. Every
// engine address comes from game_layout; the game's own sky draw runs
// through the recomp module dispatch at game_layout.hooks.sky_draw.
//
// The native renderer re-derives the sky dome from gamestate (g_Cyclorama
// + the camera matrices) in float, instead of replaying the PSX renderer's
// GPU packets. This module is the game-specific *scene extraction* half:
// it reads g_Cyclorama, unpacks the packed dome sectors, float-projects
// them (mirroring the GTE math in core/psx/gte.c), and produces a list
// of screen-space triangles. core/ consumes that list (display only).
//
// Verification (instruments over pixels): hook the dome renderer (Spyro 1
// func_8004EBA8), capture the *actual* POLY_F3/G3 prims it emits to
// the polybuf as ground truth, run the native float extraction on the same
// inputs, and compare screen positions (expect sub-pixel - float is more
// precise than the GTE's 16.16) + colours (expect exact - gouraud colours
// are copied, not projected). All headless, all numerical. Gated by
// OPENPETE_CYC_VERIFY (off => zero overhead).
//
// Determinism: this never writes PSX RAM. When verifying it calls the real
// func_8004EBA8 exactly once (behaviour-identical to the normal dispatch
// fall-through) and only READS the polybuf + g_Cyclorama afterwards.
//
// Format: func_8004EBA8's asm, corroborated by SpyroEdit
// SkySectorHeaderS1 / UpdateSky.
//
// The same extraction also serves the level-exit / balloonist cross-fade
// renderer func_8004F000 - the same dome with a DPCS colour fade - via
// cyclorama_native_extract_faded, which transition.c feeds.

#include "feature_registry.h"
#include "psx_runtime_internal.h"   /* full CPUState (gte_ctrl, a0/a1/a2, read_word) */
#include "openpete_psx.h"
#include "openpete_subtick_view.h"  /* openpete_subtick_host_span (present-time DRAWENV read) */
#include "openpete_native_render.h"  /* native scene push + enabled gate */
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "renderer/cyclorama.h"  /* this feeder's exports */
#include "renderer/cut_guard.h"  /* cut_census_on: the held background line */
#include "renderer/orchestrator.h"  /* cyclorama_native_env_clear_commit */
#include "openpete_fiber.h"           /* openpete_yield_site: the VSync wait of a repeat present */
#include "openpete_recomp_module.h"  /* the game's own sky draw, run through the module dispatch */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "renderer/sector_pvs.h"    /* the sky tree snapshot, lookup and filled step */
#include "renderer/terrain.h"       /* terrain_sector_filled_note_sky: the load-window counts */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_layout.h"          /* game_layout: the engine RAM addresses this walk reads */
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE - fiber-hook cost split */
#include <stdatomic.h>
#include "openpete_thread_roles.h"  /* sim: every static below is the sim role's */

extern void     func_8004EBA8(CPUState* cpu);  /* the real dome renderer */
extern unsigned openpete_frame;


/* ---- native scene triangle (screen-space, float) -------------------- */
typedef struct { float sx, sy; uint32_t rgb; unsigned char sat; } cyc_vert; /* rgb=low24; sat=GTE IR1/IR2 would clamp */
typedef struct { cyc_vert v[3]; int gouraud; } cyc_tri;

#define MAX_TRIS 16384
static cyc_tri s_native[MAX_TRIS] OP_GUARDED_BY(sim); /* thread-roles: built and pushed on sim */
static int     s_native_n OP_GUARDED_BY(sim);         /* thread-roles: built and pushed on sim */
static cyc_tri s_gte[MAX_TRIS] OP_GUARDED_BY(sim);    /* thread-roles: the verify capture, on sim */
static int     s_gte_n OP_GUARDED_BY(sim);            /* thread-roles: the verify capture, on sim */
/* cyc_extract_m's sector decode scratch. Its callers (the sky-draw hook, the
 * re-extract and the cross-fade dome) run on the thread that runs the guest,
 * the re-extract while the guest is parked, so they share it as they share
 * s_native; the portal-sky builder decodes into its own. */
static cyclorama_sector_buf s_cyc_decode OP_GUARDED_BY(sim); /* thread-roles: the sky feed's decode scratch */

/* ---- helpers -------------------------------------------------------- */

/** @brief Float-project one vertex by the GTE RTPS math, with no translation.
 *
 * Wraps ngte_project_raw with a NULL translation. The sat flag bins the two
 * GTE fixed-point clamps the unclamped float path departs from (IR16
 * saturation and the H >= 2*SZ3 divide cap).
 * @param M The Q12 rotation matrix.
 * @param vx Vertex X.
 * @param vy Vertex Y.
 * @param vz Vertex Z.
 * @param ofx_px Screen X offset in pixels.
 * @param ofy_px Screen Y offset in pixels.
 * @param v The projection view (H and bounds).
 * @param out_sx Receives screen X.
 * @param out_sy Receives screen Y.
 * @param out_sat Receives the clamp flag; may be NULL.
 * @return View-space Z (MAC3); the caller treats a value <= 0 as behind the
 *         camera. */
static double project(const int M[3][3], int vx, int vy, int vz,
                      double ofx_px, double ofy_px, const ngte_view* v,
                      double* out_sx, double* out_sy, int* out_sat) {
    return ngte_project_raw(M, NULL, (double)vx, (double)vy, (double)vz,
                            ofx_px, ofy_px, v, out_sx, out_sy, out_sat);
}

/* ---- near-plane clip ----------------------------------------------------
 * The sky dome is a full 360 degree surround (all sectors live in RAM); at
 * wide aspect ratio the side/wrap geometry that the canonical 4:3 path's
 * near plane rejected (whole-tri drop when any vertex is behind the camera)
 * is on screen, so it is clipped to the near plane instead: the in-front
 * part renders, the behind part is cut. Camera-space (m1,m2,m3); m3 = view-Z.
 * The cut itself is ngte_clip_lerp, the same edge crossing the terrain uses;
 * the dome carries no UVs and its plane is fixed, so its corners leave
 * u/v/oflag zero and it passes CYC_NEARZ as the plane. */
#define CYC_NEARZ 1.0

/** @brief Project a triangle's three in-front camera-space corners and append it to s_native.
 *
 * Each corner projects as ofx + m1*H/m3, ofy + m2*H/m3 (m3 of 0 divides by
 * 1). The triangle is dropped when all three corners lie left of the view's
 * xlo, right of its xhi (the widescreen-aware horizontal bounds), above 0 or
 * below 240 (vertical stays 4:3), or when s_native is full. Colours are
 * copied; the sat flags are cleared.
 * @param a First corner.
 * @param b Second corner.
 * @param c Third corner.
 * @param gouraud Nonzero for a gouraud triangle, 0 for flat.
 * @param vw The projection view. */
static void cyc_push_tri(ngte_cvert a, ngte_cvert b, ngte_cvert c, int gouraud,
                         const ngte_view* vw) OP_REQUIRES(sim) {
    const float xlo = vw->xlo, xhi = vw->xhi;
    const double ofx = vw->ofx, ofy = vw->ofy, H = vw->H_eff;
    ngte_cvert v[3] = { a, b, c }; float sx[3], sy[3];
    for (int k = 0; k < 3; k++) {
        double z = v[k].m3 != 0.0 ? v[k].m3 : 1.0;
        sx[k] = (float)(ofx + v[k].m1 * H / z);
        sy[k] = (float)(ofy + v[k].m2 * H / z);
    }
    if ((sx[0]<xlo&&sx[1]<xlo&&sx[2]<xlo) || (sx[0]>xhi&&sx[1]>xhi&&sx[2]>xhi) ||
        (sy[0]<0&&sy[1]<0&&sy[2]<0)       || (sy[0]>240&&sy[1]>240&&sy[2]>240)) return;
    if (s_native_n >= MAX_TRIS) return;
    cyc_tri* t = &s_native[s_native_n++];
    t->gouraud = gouraud;
    for (int k = 0; k < 3; k++) {
        t->v[k].sx = sx[k]; t->v[k].sy = sy[k]; t->v[k].rgb = v[k].rgb; t->v[k].sat = 0;
    }
}

/** @brief Clip a camera-space triangle to m3 >= CYC_NEARZ and push the result as a fan.
 *
 * Sutherland-Hodgman against the one plane: no corner behind yields the
 * triangle, one behind yields a quad pushed as two triangles, two behind
 * yield one triangle, all three behind yield nothing. Crossings come from
 * ngte_clip_lerp, which interpolates the colour.
 * @param a First corner.
 * @param b Second corner.
 * @param c Third corner.
 * @param gouraud Nonzero for a gouraud triangle, 0 for flat.
 * @param vw The projection view. */
static void cyc_clip_emit(ngte_cvert a, ngte_cvert b, ngte_cvert c, int gouraud,
                          const ngte_view* vw) OP_REQUIRES(sim) {
    ngte_cvert in[3] = { a, b, c }, out[4]; int nout = 0;
    for (int i = 0; i < 3; i++) {
        ngte_cvert cur = in[i], nxt = in[(i + 1) % 3];
        int ci = cur.m3 >= CYC_NEARZ, ni = nxt.m3 >= CYC_NEARZ;
        if (ci) out[nout++] = cur;
        if (ci != ni && nout < 4) out[nout++] = ngte_clip_lerp(cur, nxt, CYC_NEARZ);
    }
    if (nout < 3) return;
    cyc_push_tri(out[0], out[1], out[2], gouraud, vw);
    if (nout == 4) cyc_push_tri(out[0], out[2], out[3], gouraud, vw);
}

/* ---- native extraction ---------------------------------------------- */
static void cyc_extract_m(CPUState* cpu, uint32_t a0,
                          const int Mcull[3][3], const int Mdraw[3][3],
                          uint32_t fade_rgb, int fade_ir0) OP_REQUIRES(sim);
/** @brief Read the sky draw's cull and draw matrices from guest RAM and extract the dome into s_native.
 *
 * When the layout's sky draw takes no matrix arguments (cyc.hook_args is 0),
 * a1 and a2 are both replaced by the camera projection matrix, which the game
 * loads for the cull and the projection alike. Each matrix is read with
 * ngte_read_matrix, then cyc_extract_m does the work.
 * @param cpu The guest CPU state, read only.
 * @param a0 The draw's a0: the occlusion group index, negative for all sectors.
 * @param a1 Guest address of the cull matrix.
 * @param a2 Guest address of the draw matrix.
 * @param fade_rgb Fade target colour (low 24 bits) for the depth cue.
 * @param fade_ir0 Depth-cue factor; 0 leaves colours unfaded. */
static void cyc_extract(CPUState* cpu, uint32_t a0, uint32_t a1, uint32_t a2,
                        uint32_t fade_rgb, int fade_ir0) OP_REQUIRES(sim) {
    int Mcull[3][3], Mdraw[3][3];
    if (!game_layout.cyc.hook_args) {
        /* The draw loads the camera's projection matrix for both the cull
         * and the projection; its registers carry no matrix arguments. */
        a1 = a2 = game_layout.cam.proj_matrix;
    }
    ngte_read_matrix(cpu, a1, Mcull);
    ngte_read_matrix(cpu, a2, Mdraw);
    cyc_extract_m(cpu, a0, Mcull, Mdraw, fade_rgb, fade_ir0);
}

static uint8_t s_cyc_vis[1024] OP_GUARDED_BY(sim);   /* thread-roles: copied after the draw on sim */
static uint8_t s_cyc_game[1024] OP_GUARDED_BY(sim);  /* the game's own bytes of the same draw */
static uint32_t s_cyc_vis_n OP_GUARDED_BY(sim) = 0;  /* thread-roles: copied after the draw on sim */
/* The sky PVS tree (the tree func_80048D18(1) / func_8002D0D8(1) reads) and
 * its --sector-visibility filled state. */
static sector_pvs_tree_t   s_sky_tree OP_GUARDED_BY(sim);
static sector_pvs_filled_t s_sky_filled OP_GUARDED_BY(sim);

/** @brief Under --sector-visibility filled, adds the sky sectors C' admits to the captured bytes.
 *
 * The sky's C' is the terrain rule (sector_pvs_filled_step) over the sky
 * tree at the tick camera (cam.xform_end), with the game's bytes as the
 * verdict: a sky sector one of the 13 opposite neighbour pairs lists is
 * drawn too, and the previous tick's set joins while the camera stays
 * within one cell. Nothing changes on a game without a position-keyed PVS,
 * with the lookup flag 0 or without a sky tree (the game then draws every
 * sky sector). Writes host memory only.
 * @param cpu The guest CPU state, read only.
 * @param n The captured byte count. */
static void cyc_filled_visibility(CPUState* cpu, uint32_t n) OP_REQUIRES(sim) {
    if (openpete_native_sector_visibility_active() != OPENPETE_SECTOR_VIS_FILLED
        || !game_layout.env.vis_aspect_independent) return;
    sector_pvs_tree_refresh(&s_sky_tree, cpu, cpu->read_word(game_layout.cyc.occ_groups));
    if (!s_sky_tree.b) { sector_pvs_filled_reset(&s_sky_filled); return; }
    const uint32_t m = n < 256u ? n : 256u;
    uint8_t base[256] = { 0 };
    for (uint32_t i = 0; i < m; i++) base[i] = s_cyc_game[i] ? 0xFF : 0;
    const uint32_t xe = game_layout.cam.xform_end;
    const int32_t p[3] = { (int32_t)cpu->read_word(xe), (int32_t)cpu->read_word(xe + 4u),
                           (int32_t)cpu->read_word(xe + 8u) };
    const int r = sector_pvs_filled_step(&s_sky_filled, &s_sky_tree, p, m, base);
    int added = 0;
    for (uint32_t i = 0; i < m; i++) {
        added += s_sky_filled.u[i] && !base[i];
        if (s_sky_filled.u[i]) s_cyc_vis[i] = 0xFF;
    }
    terrain_sector_filled_note_sky(r & SECTOR_PVS_COMPUTED, added);
}

/** @brief Copy the sky's per-sector visibility bytes out of guest scratch memory.
 *
 * The bytes (game_layout.cyc.visibility, one per sector, nonzero = drawn) are
 * copied right after the canonical draw because the game reuses that scratch
 * range later in the tick; a sub-tick re-extract reads the copy in s_cyc_vis.
 * The count is the sector count clamped to 1024. A layout without a
 * visibility range leaves the count at 0. Under --sector-visibility frustum
 * every sector is marked drawn; under filled the copy gains the sectors
 * cyc_filled_visibility admits; s_cyc_game keeps the game's bytes.
 * @param cpu The guest CPU state, read only. */
static void cyc_capture_visibility(CPUState* cpu) OP_REQUIRES(sim) {
    s_cyc_vis_n = 0;
    if (!game_layout.cyc.visibility) return;
    uint32_t n = cpu->read_word(game_layout.cyc.sector_count);
    if (n > sizeof s_cyc_vis) n = sizeof s_cyc_vis;
    for (uint32_t i = 0; i < n; i++) s_cyc_vis[i] = s_cyc_game[i] = cpu->read_byte(game_layout.cyc.visibility + i);
    s_cyc_vis_n = n;
    /* frustum draws every sky sector in front of the camera, as a view wider
     * than 4:3 does in any mode: the radius cull and the depth test decide. */
    if (openpete_native_sector_visibility_active() == OPENPETE_SECTOR_VIS_FRUSTUM) memset(s_cyc_vis, 0xFF, n);
    cyc_filled_visibility(cpu, n);
}

/** @brief Prints one [sky-vis] census line per canonical sky draw under OPENPETE_SKY_VIS_CENSUS=1.
 *
 * Compares the host lookup of the sky tree (game_layout.cyc.occ_groups, the
 * tree func_80048D18(1) / func_8002D0D8(1) reads) at the tick camera
 * (cam.xform_end) against the bytes the game wrote (s_cyc_game, captured
 * after the draw), and counts the sectors C' (the opposite-pair rule of
 * --sector-visibility filled) adds to the game's set. With no tree or a
 * zero lookup flag the game draws every sector and the host set is all.
 * Reads guest RAM only.
 * @param cpu The guest CPU state, read only. */
static void cyc_vis_census(CPUState* cpu) OP_REQUIRES(sim) {
    static int on OP_GUARDED_BY(sim) = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_SKY_VIS_CENSUS"); on = e && *e == '1'; }
    if (!on || !game_layout.env.vis_aspect_independent || !game_layout.cyc.visibility) return;
    sector_pvs_tree_refresh(&s_sky_tree, cpu, cpu->read_word(game_layout.cyc.occ_groups));
    const uint32_t xe = game_layout.cam.xform_end;
    const int32_t p[3] = { (int32_t)cpu->read_word(xe), (int32_t)cpu->read_word(xe + 4u),
                           (int32_t)cpu->read_word(xe + 8u) };
    uint8_t host[256], cp[256];
    int open = 1;
    if (s_sky_tree.b) sector_pvs_lookup(s_sky_tree.b, s_sky_tree.len, 0, p, host, &open);
    else memset(host, 0xFF, sizeof host);
    const uint32_t n = s_cyc_vis_n < 256u ? s_cyc_vis_n : 256u;
    memcpy(cp, host, sizeof cp);
    if (s_sky_tree.b && !open) {
        uint8_t nb[SECTOR_PVS_RING][256], op[SECTOR_PVS_RING];
        sector_pvs_ring(s_sky_tree.b, s_sky_tree.len, 0, p, nb, op);
        sector_pvs_pairs((const uint8_t (*)[256])nb, op, 1, n, cp);
    }
    int mismatch = 0, nh = 0, ng = 0, extra = 0;
    for (uint32_t i = 0; i < n; i++) {
        const int g = s_cyc_game[i] != 0, h = host[i] != 0;
        mismatch += g != h;
        nh += h;
        ng += g;
        extra += cp[i] && !h;
    }
    fprintf(stderr, "[sky-vis] frame=%u level=%u cell=%d,%d,%d sectors=%u tree=%d open=%d host=%d guest=%d "
                    "mismatch=%d c2_extra=%d\n",
            openpete_frame, (unsigned)s_sky_tree.level, p[0] >> 11, p[1] >> 11, p[2] >> 11, n, s_sky_tree.b != NULL, open,
            nh, ng, mismatch, extra);
}

/** @brief Passes one camera-space sky face to cyc_clip_emit under the view in `ctx`.
 * @param a The first corner.
 * @param b The second corner.
 * @param c The third corner.
 * @param gouraud Nonzero for a gouraud face.
 * @param ctx The ngte_view the face projects under. */
static void cyc_emit_cb(ngte_cvert a, ngte_cvert b, ngte_cvert c, int gouraud, void* ctx) OP_REQUIRES(sim) {
    cyc_clip_emit(a, b, c, gouraud, (const ngte_view*)ctx);
}

/** @brief Builds the sector-pointer iteration set of one sky draw (func_8004EBA8 phase 0).
 *
 * A layout with per-sector visibility bytes takes the sectors whose captured
 * byte is nonzero; otherwise a negative a0 takes every sector and a
 * non-negative one the occlusion group occGroups[a0] (indices ending at
 * 0xFF). In widescreen (`ws`) every sector is taken: the visibility bytes and
 * the group are a 4:3 selection that omits the dome sectors visible only at
 * the widened sides, and the near-plane clip and the scissor confine the
 * full 360 degree dome to the visible frustum. The count is clamped to 1024.
 * @param cpu The guest CPU state, read only.
 * @param a0 The draw's a0: the occlusion group index, negative for all sectors.
 * @param ws Nonzero when the view is wider than 4:3.
 * @param sectors Receives the sector addresses.
 * @return The number of sectors written. */
static uint32_t cyc_sector_list(CPUState* cpu, uint32_t a0, int ws, uint32_t sectors[1024]) OP_REQUIRES_SHARED(sim) {
    uint32_t sectorCount = cpu->read_word(game_layout.cyc.sector_count);
    const uint32_t sectorsBase = cpu->read_word(game_layout.cyc.sectors);
    const uint32_t occGroups   = cpu->read_word(game_layout.cyc.occ_groups);
    uint32_t n = 0;
    if (sectorCount > 1024) sectorCount = 1024;
    if (game_layout.cyc.visibility || (int32_t)a0 < 0 || ws) {
        /* The visibility bytes: nonzero = drawn, written by the game's
         * visibility pass before the draw, keyed on the camera position
         * against the sky occlusion data. */
        for (uint32_t i = 0; i < sectorCount; i++)
            if (!game_layout.cyc.visibility || ws || (i < s_cyc_vis_n && s_cyc_vis[i]))
                sectors[n++] = cpu->read_word(sectorsBase + i * 4);
        return n;
    }
    const uint32_t list = occGroups ? cpu->read_word(occGroups + a0 * 4) : 0u;
    for (uint32_t k = 0; list && k < 1024; k++) {
        const uint8_t idx = cpu->read_byte(list + k);
        if (idx == 0xFF) break;
        sectors[n++] = cpu->read_word(sectorsBase + (uint32_t)idx * 4);
    }
    return n;
}

/** @brief Rebuild s_native from g_Cyclorama with the given cull and draw matrices.
 *
 * Reads the sector table and the camera GTE offsets and H, then mirrors
 * func_8004EBA8: builds the sector list (cyc_sector_list), culls each
 * sector against its radius with the cull matrix, and decodes it with the
 * draw matrix through cyclorama_decode_sector, each face near-plane clipped
 * through cyc_clip_emit. Widescreen views skip the radius cull. When the
 * layout depth-cues in the draw and no fade is passed, a nonzero fog factor
 * word supplies the fade. Reads only.
 * @param cpu The guest CPU state, read only.
 * @param a0 The draw's a0: the occlusion group index, negative for all sectors.
 * @param Mcull Q12 matrix for the sector radius cull.
 * @param Mdraw Q12 matrix for the vertex rotation.
 * @param fade_rgb Fade target colour (low 24 bits) for the depth cue.
 * @param fade_ir0 Depth-cue factor; 0 leaves colours unfaded. */
static void cyc_extract_m(CPUState* cpu, uint32_t a0,
                          const int Mcull[3][3], const int Mdraw[3][3],
                          uint32_t fade_rgb, int fade_ir0) OP_REQUIRES(sim) {
    s_native_n = 0;
    /* One projection view for this pass: offsets, H and the Hor+ bounds. The
     * cull/draw matrices come from the hook's a1/a2, not the view. */
    ngte_view vw; ngte_view_build(cpu, &vw);
    const int ws = vw.extended;   /* Hor+ or a wider FOV */
    uint32_t sectors[1024];
    const uint32_t nSectors = cyc_sector_list(cpu, a0, ws, sectors);

    /* A sky draw that depth-cues its own colours: the factor word, when
     * nonzero, is IR0 against the fog far colour for every colour entry. */
    if (!fade_ir0 && game_layout.cyc.fade_in_draw && game_layout.fog.factor) {
        int ir0 = (int)cpu->read_word(game_layout.fog.factor);
        if (ir0) { fade_ir0 = ir0; fade_rgb = ngte_fog_far_colour(cpu); }
    }
    const cyclorama_fade fade[2] = { { fade_rgb, fade_ir0 }, { 0u, 0 } };
    const cyclorama_src src = { cpu, NULL, 0u, 0u };
    double Md[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) Md[i][j] = (double)Mdraw[i][j];

    for (uint32_t si = 0; si < nSectors; si++) {
        const uint32_t S = sectors[si];
        if (!S) continue;

        /* phase-1 cull: project the representative point with the cull
         * matrix; visible iff MAC3 > radius. SKIPPED in widescreen - this is a
         * 4:3-frustum range/behind cull that drops side sectors which are
         * partially visible at wide FOV; the near-plane clip + trivial reject
         * confine the kept geometry correctly instead. */
        if (!ws) {
            uint32_t c0 = cpu->read_word(S + 0x00), c1 = cpu->read_word(S + 0x04);
            int cull_vx = (int16_t)(c0 & 0xFFFF), cull_vy = (int16_t)(c0 >> 16);
            int cull_vz = (int16_t)(c1 >> 16);
            int radius  = (int16_t)(c1 & 0xFFFF);
            double dsx, dsy;
            double cull_mac3 = project(Mcull, cull_vx, cull_vy, cull_vz, 0, 0, &vw, &dsx, &dsy, NULL);
            if (cull_mac3 <= (double)radius) continue;
        }
        cyclorama_decode_sector(&s_cyc_decode, &src, S, Md, 4096.0, fade, cyc_emit_cb, &vw);
    }
}

/* ---- the sector decode, shared with the portal previews ---------------- */

/** @brief Reads one little-endian word of a sector source, 0 outside a host source's span.
 * @param s The source.
 * @param a The address.
 * @return The word. */
static uint32_t cyc_rd32(const cyclorama_src* s, uint32_t a) {
    if (!s->buf) return s->cpu->read_word(a);
    const uint32_t o = a - s->base;
    if (a < s->base || o > s->len || s->len - o < 4u) return 0u;
    const uint8_t* b = s->buf + o;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/** @brief Reads one little-endian halfword of a sector source, 0 outside a host source's span.
 * @param s The source.
 * @param a The address.
 * @return The halfword. */
static uint32_t cyc_rd16(const cyclorama_src* s, uint32_t a) {
    if (!s->buf) return s->cpu->read_half(a);
    const uint32_t o = a - s->base;
    if (a < s->base || o > s->len || s->len - o < 2u) return 0u;
    return (uint32_t)s->buf[o] | ((uint32_t)s->buf[o + 1] << 8);
}

/** @brief Emits one face of a decoded sector by its vertex and colour indices.
 *
 * A face with a vertex index at or past the vertex count is dropped; a colour
 * index at or past the colour count reads black. The face is flat when its
 * three colour indices are equal, else gouraud.
 * @param D The decoded sector.
 * @param v The three vertex indices.
 * @param k The three colour indices.
 * @param fn Receives the face.
 * @param ctx The receiver's context. */
static void cyc_sector_face(const cyclorama_sector_buf* D, const uint32_t v[3], const uint32_t k[3],
                            cyclorama_tri_fn fn, void* ctx) {
    if (v[0] >= D->nv || v[1] >= D->nv || v[2] >= D->nv) return;
    const int gouraud = !(k[0] == k[1] && k[0] == k[2]);
    ngte_cvert C[3];
    for (int i = 0; i < 3; i++) {
        const uint32_t ki = gouraud ? k[i] : k[0];
        memset(&C[i], 0, sizeof C[i]);
        C[i].m1 = D->m1[v[i]]; C[i].m2 = D->m2[v[i]]; C[i].m3 = D->m3[v[i]];
        C[i].rgb = ki < D->nc ? D->col[ki] : 0;
    }
    fn(C[0], C[1], C[2], gouraud, ctx);
}

/** @brief Walks a Spyro 2/3 sector's strips: one face word per strip and a halfword index list.
 *
 * Face word bits 31-24 are the third vertex, 23-17 / 16-10 / 9-3 the three
 * colour indices and 2-0 the number of continuation triangles; the strip's
 * first halfword holds the first two vertices ((H >> 8) & 0xFF, H & 0xFF)
 * and each continuation halfword adds one vertex (H & 0xFF) with one colour
 * ((H >> 8) & 0x7F); bit 15 of a continuation shifts the window by one,
 * clear keeps the middle vertex and replaces the first.
 * @param s The source.
 * @param faceBase The face-word list's address.
 * @param hF The sector's +0x10 word: face-word list length << 16 | halfword list length.
 * @param D The decoded sector.
 * @param fn Receives each face.
 * @param ctx The receiver's context. */
static void cyc_sector_strips(const cyclorama_src* s, uint32_t faceBase, uint32_t hF,
                              const cyclorama_sector_buf* D, cyclorama_tri_fn fn, void* ctx) {
    uint32_t hw = faceBase + (hF >> 16);
    const uint32_t hwEnd = hw + (hF & 0xFFFF);
    uint32_t fw = faceBase;
    for (int guard = 0; hw + 2 <= hwEnd && guard < 8192; guard++) {
        const uint32_t W = cyc_rd32(s, fw), H = cyc_rd16(s, hw);
        fw += 4; hw += 2;
        uint32_t v[3] = { (H >> 8) & 0xFF, H & 0xFF, (W >> 24) & 0xFF };
        uint32_t k[3] = { (W >> 17) & 0x7F, (W >> 10) & 0x7F, (W >> 3) & 0x7F };
        uint32_t more = W & 7;
        for (;;) {
            cyc_sector_face(D, v, k, fn, ctx);
            if (!more || hw + 2 > hwEnd) break;
            more--;
            const uint32_t H2 = cyc_rd16(s, hw);
            hw += 2;
            if (H2 & 0x8000) { v[0] = v[1]; v[1] = v[2]; k[0] = k[1]; k[1] = k[2]; }
            else             { v[0] = v[2]; k[0] = k[2]; }
            v[2] = H2 & 0xFF; k[2] = (H2 >> 8) & 0x7F;
        }
    }
}

void cyclorama_decode_sector(cyclorama_sector_buf* D, const cyclorama_src* src, uint32_t S,
                             const double M[3][3], double scale, const cyclorama_fade fade[2],
                             cyclorama_tri_fn fn OP_SYNC_FN, void* ctx) {
    const uint32_t h8 = cyc_rd32(src, S + 0x08), hC = cyc_rd32(src, S + 0x0C);
    const uint32_t hF = cyc_rd32(src, S + 0x10);
    const int centerX = (int16_t)(h8 & 0xFFFF), centerY = (int16_t)(h8 >> 16);
    const int centerZ = (int16_t)(hC >> 16);
    /* Spyro 1: +0x0C low half = vertex count, +0x10 = face count (bits
     * 0-12) and colour-table byte length (>> 14). Spyro 2/3: +0x0C byte 0 =
     * vertex count, byte 1 = colour count; +0x10 high half = face-word
     * list byte length, low half = index-halfword list byte length. */
    const int s3 = game_layout.cyc.face_layout == CYC_FACES_SPYRO3;
    uint32_t vertexCount = s3 ? (hC & 0xFF) : (hC & 0xFFFF);
    uint32_t colorCount  = s3 ? ((hC >> 8) & 0xFF) : (hF >> 14) / 4;
    if (vertexCount > 4096) vertexCount = 4096;
    if (colorCount > 4096) colorCount = 4096;
    const uint32_t vertBase  = S + game_layout.cyc.vertex_base;
    const uint32_t colorBase = vertBase + vertexCount * 4;
    const uint32_t faceBase  = colorBase + (s3 ? colorCount * 4 : (hF >> 14));
    /* The camera-space MAC of every vertex: the near-plane clip interpolates
     * in camera space, then projects. */
    for (uint32_t i = 0; i < vertexCount; i++) {
        const uint32_t w = cyc_rd32(src, vertBase + i * 4);
        const int vx = centerX - (int)((w >> 10) & 0x7FF);
        const int vy = centerY - (int)(w & 0x3FF);
        const int vz = centerZ + (int)(w >> 21);
        D->m1[i] = (M[0][0]*(double)vx + M[0][1]*(double)vy + M[0][2]*(double)vz) / scale;
        D->m2[i] = (M[1][0]*(double)vx + M[1][1]*(double)vy + M[1][2]*(double)vz) / scale;
        D->m3[i] = (M[2][0]*(double)vx + M[2][1]*(double)vy + M[2][2]*(double)vz) / scale;
    }
    for (uint32_t j = 0; j < colorCount; j++) {
        uint32_t c = cyc_rd32(src, colorBase + j * 4) & 0xFFFFFF;
        for (int f = 0; f < 2; f++)
            if (fade[f].ir0) c = ngte_depth_cue(c, fade[f].rgb, fade[f].ir0);
        D->col[j] = c;
    }
    D->nv = vertexCount; D->nc = colorCount;
    if (s3) { cyc_sector_strips(src, faceBase, hF, D, fn, ctx); return; }
    /* Faces: word0 = vertex indices, word1 = colour indices. */
    const uint32_t faceCount = hF & 0x1FFF;
    for (uint32_t f = 0; f < faceCount; f++) {
        const uint32_t w0 = cyc_rd32(src, faceBase + f * 8), w1 = cyc_rd32(src, faceBase + f * 8 + 4);
        const uint32_t v[3] = { (w0 >> 22) & 0x3FF, (w0 >> 12) & 0xFF, (w0 >> 2) & 0xFF };
        const uint32_t k[3] = { (w1 >> 22) & 0x3FF, (w1 >> 12) & 0xFF, (w1 >> 2) & 0xFF };
        cyc_sector_face(D, v, k, fn, ctx);
    }
}

/* ---- ground-truth capture: decode the prims func_8004EBA8 emitted ---- */
/* Prims are contiguous in [start,end). POLY_F3 = 0x14 bytes
 * {next,rgb,xy0,xy1,xy2}; POLY_G3 = 0x1C bytes
 * {next,rgb0,xy0,rgb1,xy1,rgb2,xy2}. word0 high byte distinguishes them
 * (confirmed by the hex dump below). */
/** @brief Split a packed prim vertex word into signed 16-bit screen X (low half) and Y (high half).
 *
 * @param xy The packed word.
 * @param x Receives X.
 * @param y Receives Y. */
static void unpack_xy(uint32_t xy, float* x, float* y) {
    *x = (float)(int16_t)(xy & 0xFFFF);
    *y = (float)(int16_t)(xy >> 16);
}
/** @brief Decode the POLY_F3 and POLY_G3 prims in a guest range into s_gte.
 *
 * Walks [start, end) word by word: a word whose high byte is 0x06 starts a
 * gouraud prim (0x1C bytes), 0x04 a flat prim (0x14 bytes), anything else is
 * skipped 4 bytes. Colours keep the low 24 bits and positions unpack as signed
 * halves. Decoding stops storing at MAX_TRIS triangles and the walk at
 * MAX_TRIS iterations, resync steps included. Resets s_gte_n.
 * @param cpu The guest CPU state, read only.
 * @param start Start of the prim interval.
 * @param end End of the prim interval. */
static void capture_gte(CPUState* cpu, uint32_t start, uint32_t end) OP_REQUIRES(sim) {
    s_gte_n = 0;
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < MAX_TRIS) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t code = w0 >> 24;            /* prim type byte (see dump) */
        int gouraud;
        if (code == 0x06) gouraud = 1;
        else if (code == 0x04) gouraud = 0;
        else { p += 4; continue; }           /* resync if layout differs */
        cyc_tri* t = &s_gte[s_gte_n];
        if (s_gte_n < MAX_TRIS) {
            if (gouraud) {
                t->gouraud = 1;
                t->v[0].rgb = cpu->read_word(p + 0x04) & 0xFFFFFF;
                unpack_xy(cpu->read_word(p + 0x08), &t->v[0].sx, &t->v[0].sy);
                t->v[1].rgb = cpu->read_word(p + 0x0C) & 0xFFFFFF;
                unpack_xy(cpu->read_word(p + 0x10), &t->v[1].sx, &t->v[1].sy);
                t->v[2].rgb = cpu->read_word(p + 0x14) & 0xFFFFFF;
                unpack_xy(cpu->read_word(p + 0x18), &t->v[2].sx, &t->v[2].sy);
            } else {
                uint32_t rgb = cpu->read_word(p + 0x04) & 0xFFFFFF;
                t->gouraud = 0;
                t->v[0].rgb = t->v[1].rgb = t->v[2].rgb = rgb;
                unpack_xy(cpu->read_word(p + 0x08), &t->v[0].sx, &t->v[0].sy);
                unpack_xy(cpu->read_word(p + 0x0C), &t->v[1].sx, &t->v[1].sy);
                unpack_xy(cpu->read_word(p + 0x10), &t->v[2].sx, &t->v[2].sy);
            }
            s_gte_n++;
        }
        p += gouraud ? 0x1C : 0x14;
    }
}

/* ---- comparison ----------------------------------------------------- */
/** @brief Whether a captured GTE vertex lies inside the 512x240 display area with a 1-pixel margin.
 *
 * compare_and_report bins off-screen vertices separately: there the GTE's
 * fixed-point clamps bite and the unclamped float projection departs from
 * them by design.
 * @param v The vertex, in the GTE's integer screen coordinates.
 * @return 1 when -1 <= sx <= 513 and -1 <= sy <= 241, else 0. */
static int onscreen(const cyc_vert* v) {
    return v->sx >= -1.f && v->sx <= 513.f && v->sy >= -1.f && v->sy <= 241.f;
}
/** @brief Compare s_gte against s_native and print one statistics line to stderr.
 *
 * Each GTE triangle is paired greedily with the nearest unused native
 * triangle by the summed squared vertex distance (vertex k to vertex k); a
 * pair farther than 3*96*96 is dropped. Over the pairs it counts exact colour
 * matches and flat/gouraud disagreements, and over on-screen vertices whose
 * native projection did not saturate it reports the mean, maximum and signed
 * X error, the floor-match rate within one pixel, and the worst vertex.
 * Off-screen or saturated vertices are counted separately. Pairing is
 * quadratic in the triangle counts. */
static void compare_and_report(void) OP_REQUIRES(sim) {
    /* Match gte->native greedily by the summed per-vertex distance; the
     * nearest unused native triangle wins. */
    static int used[MAX_TRIS] OP_GUARDED_BY(sim);   /* thread-roles: the verify report runs on sim */
    memset(used, 0, sizeof(int) * (s_native_n > 0 ? s_native_n : 1));
    int matched = 0, color_exact = 0, fg_disagree = 0;
    /* on-screen, non-saturated verts: the visible result, expected ~exact */
    int on_n = 0;       double on_sum = 0.0, on_max = 0.0;
    /* floor-truncation check: GTE = floor(continuous); so floor(native)
     * should equal GTE within +-1 (reciprocal rounding) for on-screen */
    int floor_ok = 0;   double signed_sum = 0.0;
    int off_n = 0;
    double worst = -1.0; float wnx=0,wny=0,wgx=0,wgy=0;
    for (int g = 0; g < s_gte_n; g++) {
        /* Score candidates by per-vertex distance sum (the native vertex order
         * matches the asm's emit order o0->xy0,o1->xy1,o2->xy2, so v[k]<->v[k]),
         * which separates neighbouring triangles a centroid match would
         * mispair. */
        int best = -1; float bestd = 1e18f;
        for (int n = 0; n < s_native_n; n++) {
            if (used[n]) continue;
            float d = 0.f;
            for (int k = 0; k < 3; k++) {
                float dx = s_gte[g].v[k].sx - s_native[n].v[k].sx;
                float dy = s_gte[g].v[k].sy - s_native[n].v[k].sy;
                d += dx*dx + dy*dy;
            }
            if (d < bestd) { bestd = d; best = n; }
        }
        if (best < 0 || bestd > 3.f*(96.0f*96.0f)) continue;
        used[best] = 1; matched++;
        const cyc_tri *G = &s_gte[g], *N = &s_native[best];
        if (G->v[0].rgb==N->v[0].rgb && G->v[1].rgb==N->v[1].rgb && G->v[2].rgb==N->v[2].rgb)
            color_exact++;
        if (G->gouraud != N->gouraud) fg_disagree++;
        for (int k = 0; k < 3; k++) {
            double dx = N->v[k].sx - G->v[k].sx, dy = N->v[k].sy - G->v[k].sy;
            double dist = sqrt(dx*dx + dy*dy);
            /* exact-expected = on-screen AND the GTE IR clamp didn't bite;
             * everything else is the sanctioned float/GTE saturation divergence. */
            if (onscreen(&G->v[k]) && !N->v[k].sat) {
                on_n++; on_sum += dist; if (dist > on_max) on_max = dist;
                signed_sum += dx;
                if (fabs(floor(N->v[k].sx) - G->v[k].sx) <= 1.0 &&
                    fabs(floor(N->v[k].sy) - G->v[k].sy) <= 1.0) floor_ok++;
                if (dist > worst) { worst = dist; wnx=N->v[k].sx; wny=N->v[k].sy; wgx=G->v[k].sx; wgy=G->v[k].sy; }
            } else off_n++;
        }
    }
    fprintf(stderr,
        "[cyc-verify f=%u] gte=%d native=%d matched=%d | ONSCREEN verts=%d "
        "mean=%.3fpx max=%.3fpx floor-match(+-1)=%d/%d(%.1f%%) bias=%.2fpx | "
        "offscreen(saturated)=%d | colour-exact=%d/%d(%.1f%%) F3/G3-disagree=%d | "
        "worst-onscreen native(%.1f,%.1f)vs gte(%.0f,%.0f)\n",
        openpete_frame, s_gte_n, s_native_n, matched,
        on_n, on_n?on_sum/on_n:0.0, on_max, floor_ok, on_n,
        on_n?100.0*floor_ok/on_n:0.0, on_n?signed_sum/on_n:0.0,
        off_n, color_exact, matched, matched?100.0*color_exact/matched:0.0,
        fg_disagree, wnx,wny,wgx,wgy);
}

/* ---- one-shot format dump ------------------------------------------- */
/** @brief Print the sky draw's arguments, the GTE offsets and H, the cyclorama header, the first sector's layout and the first prim words to stderr.
 *
 * The sector fields are decoded with the Spyro 1 layout (vertex count in the
 * low half of +0x0C, colour-table byte length in +0x10 >> 14, face count in
 * its low 13 bits). At most 32 words of the prim interval are printed.
 * @param cpu The guest CPU state, read only.
 * @param a0 The draw's a0 (occlusion group index).
 * @param a1 The cull matrix address.
 * @param a2 The draw matrix address.
 * @param start Start of the prim interval the draw wrote.
 * @param end End of the prim interval. */
static void hex_dump(CPUState* cpu, uint32_t a0, uint32_t a1, uint32_t a2,
                     uint32_t start, uint32_t end) {
    fprintf(stderr, "[cyc-dump f=%u] a0=%d a1=%08X a2=%08X OFX=%d OFY=%d H=%u\n",
            openpete_frame, (int32_t)a0, a1, a2,
            (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16,
            cpu->gte_ctrl[26] & 0xFFFF);
    fprintf(stderr, "  g_Cyclorama: count=%u sectors=%08X occCnt=%u occGrp=%08X bg=%08X\n",
            cpu->read_word(game_layout.cyc.sector_count), cpu->read_word(game_layout.cyc.sectors),
            cpu->read_word(game_layout.cyc.occ_group_count), cpu->read_word(game_layout.cyc.occ_groups),
            cpu->read_word(game_layout.cyc.bg_colour));
    uint32_t s0 = cpu->read_word(cpu->read_word(game_layout.cyc.sectors)); /* first sector ptr */
    fprintf(stderr, "  sector0=%08X hdr: ", s0);
    for (int i = 0; i <= 0x14; i += 4) fprintf(stderr, "[%02X]=%08X ", i, cpu->read_word(s0+i));
    fprintf(stderr, "\n  sector0 verts: ");
    for (int i = 0; i < 4; i++) fprintf(stderr, "%08X ", cpu->read_word(s0+game_layout.cyc.vertex_base+i*4));
    uint32_t vc = cpu->read_word(s0+0xC) & 0xFFFF;
    uint32_t cb = s0+game_layout.cyc.vertex_base+vc*4;
    fprintf(stderr, "\n  sector0 colours@%08X: ", cb);
    for (int i = 0; i < 4; i++) fprintf(stderr, "%08X ", cpu->read_word(cb+i*4));
    uint32_t foff = cpu->read_word(s0+0x10) >> 14;
    uint32_t fb = cb + foff;
    fprintf(stderr, "\n  sector0 faces@%08X (foff=%u fcnt=%u): ", fb, foff, cpu->read_word(s0+0x10)&0x1FFF);
    for (int i = 0; i < 4; i++) fprintf(stderr, "%08X ", cpu->read_word(fb+i*4));
    fprintf(stderr, "\n  prim region [%08X,%08X) %u bytes:\n   ", start, end, end-start);
    int n = (int)((end - start) / 4); if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "%08X ", cpu->read_word(start + i*4));
        if ((i & 7) == 7) fprintf(stderr, "\n   ");
    }
    fprintf(stderr, "\n");
}

/* ---- dispatch hook -------------------------------------------------- */
/** @brief Whether OPENPETE_CYC_VERIFY is set, non-empty and not starting with '0'; read once and cached.
 *
 * @return 1 when the float-versus-GTE sky comparison is armed, else 0. */
static int cyc_verify_enabled(void) {
    OP_ROLE_ASSERT(sim);
    static int s OP_GUARDED_BY(sim) = -1;   /* thread-roles: read by the hook bracket on sim */
    if (s < 0) { const char* v = getenv("OPENPETE_CYC_VERIFY"); s = (v && v[0] && v[0] != '0') ? 1 : 0; }
    return s;
}
/** @brief First VBL at which the float-versus-GTE comparison runs.
 *
 * Read once from OPENPETE_CYC_VERIFY_FRAME with atoi (default 2000) and cached.
 * @return The VBL number. */
static unsigned cyc_verify_frame(void) OP_REQUIRES(sim) {
    static unsigned f OP_GUARDED_BY(sim) = 0;  /* thread-roles: read by the sky-draw hook on sim */
    static int init OP_GUARDED_BY(sim) = 0;    /* thread-roles: read by the sky-draw hook on sim */
    if (!init) { const char* v = getenv("OPENPETE_CYC_VERIFY_FRAME"); f = v ? (unsigned)atoi(v) : 2000u; init = 1; }
    return f;
}

/** @brief Begin the native scene for this VBL, set its background colour, and push the extracted dome triangles.
 *
 * The PSX cyclorama clears to g_Cyclorama.m_BackgroundColor before drawing
 * the dome, so uncovered areas show that colour. The background is a scene
 * property and is set even when --render-paths excludes the dome, so an
 * isolated path renders over the real background as PsyCross does; only the
 * triangles are gated on RP_CYCLORAMA. PSX colour low-24 is byte0=R,
 * byte1=G, byte2=B, the RHI rgba byte order; alpha is set opaque. Triangles
 * go to the SKY group at the far view Z, untextured and opaque, tagged
 * OPENPETE_TAG_SKY(0). The first non-empty push logs once.
 * @param cpu The guest CPU state, read only. */
static void push_scene(CPUState* cpu) OP_REQUIRES(sim) {
    static int s_logged OP_GUARDED_BY(sim) = 0;   /* thread-roles: the push runs on sim */
    if (!s_logged && s_native_n > 0) {
        s_logged = 1;
        fprintf(stderr, "[cyc-native] feeding panel 0: %d cyclorama tris from gamestate (frame %u)\n",
                s_native_n, openpete_frame);
    }
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);
    /* The PSX cyclorama clears the frame to g_Cyclorama.m_BackgroundColor
     * (+0x10) before drawing the dome; mirror that so uncovered areas (below
     * the dome / gaps) show it, not black. Same colour conversion as the dome
     * (low 24 = R,G,B; opaque alpha). **The background COLOUR is a scene
     * property, not part of the cyclorama dome geometry** - set it always (even
     * when `--render-paths` excludes the dome), so an isolated path (e.g.
     * `--render-paths spyro`) renders over the real scene background, matching
     * PsyCross (which always clears to it via the draw-env). Only the dome
     * TRIANGLES are gated on RP_CYCLORAMA. */
    uint32_t bg = cyclorama_native_bg_word(cpu) & 0xFFFFFF;
    openpete_native_scene_set_bg(bg | 0xFF000000u);
    if (!render_path_enabled(RP_CYCLORAMA)) return;  /* bg only; dome not selected */
    /* Whole-dome identity: the feeder walks a flat tri list with no
     * region or band structure, so the tag names the channel and nothing
     * finer. Anything per-region has to derive a key from the vertex run
     * first. */
    openpete_native_scene_set_tag(OPENPETE_TAG_SKY(0));
    for (int i = 0; i < s_native_n; i++) {
        openpete_scene_tri t = {0};
        for (int k = 0; k < 3; k++) {
            t.v[k].sx   = s_native[i].v[k].sx;
            t.v[k].sy   = s_native[i].v[k].sy;
            /* sky = the backdrop: always behind depth-tested actors */
            t.v[k].z    = OPENPETE_DEPTH_VIEWZ_FAR;
            t.v[k].pvz  = OPENPETE_DEPTH_VIEWZ_FAR;
            t.v[k].u    = 0.0f; t.v[k].v = 0.0f;
            t.v[k].rgba = s_native[i].v[k].rgb | 0xFF000000u;
        }
        t.textured = 0;   /* cyclorama is gouraud-shaded (no texture) */
        t.tpage_x = t.tpage_y = t.clut_x = t.clut_y = t.bpp = 0;
        t.blend = 0;      /* opaque */
        /* SKY, not WORLD: drawn identically (opaque, depth-tested,
         * first) but without the WORLD group's stencil coverage MARK - the sky
         * showing through a terrain seam is exactly what the LP hole-fill
         * layer must be allowed to repaint. */
        openpete_native_scene_add_tri(&t, OPENPETE_SCENE_SKY, 0);
    }
    openpete_native_scene_set_tag(0);
}

/* Camera-matrix args from the last real render (cyc_extract reads the matrices
 * at a1/a2; they persist in RAM across the tick). Reused by the sub-tick
 * re-extract, which has no register context. */
static uint32_t s_cyc_a0 OP_GUARDED_BY(sim) = 0xFFFFFFFFu; /* thread-roles: written by cyc_pre on sim */
static uint32_t s_cyc_a1 OP_GUARDED_BY(sim) = 0;           /* thread-roles: written by cyc_pre on sim */
static uint32_t s_cyc_a2 OP_GUARDED_BY(sim) = 0;           /* thread-roles: written by cyc_pre on sim */

/* The two matrices themselves, snapshotted prev+curr per canonical tick. In
 * gameplay a1/a2 are g_Camera's own view/projection fields, which the spine's
 * camera bracket interpolates in place, so the live read above is already
 * sub-tick. During the level transition (gamestate 1) the game builds both
 * matrices on its STACK each tick - camera yaw minus a decaying spin, roll,
 * pitch, a 320/512-scaled projection copy - and the stack is neither an
 * interpolated region nor stable memory, so a pointer read there returns
 * one tick's matrices for every present of the interval. The re-extract
 * lerps this pair instead (componentwise, like the camera's lerp_matrix):
 * camera motion and spin are both baked into the two endpoints, so both
 * interpolate with no extrapolation. prev is valid only when the preceding
 * canonical tick also drew the dome from the same kind of source. */
static int      s_cyc_prev[2][3][3] OP_GUARDED_BY(sim);     /* thread-roles: rolled by cyc_pre on sim */
static int      s_cyc_curr[2][3][3] OP_GUARDED_BY(sim);     /* thread-roles: rolled by cyc_pre on sim */
static int      s_cyc_prev_valid OP_GUARDED_BY(sim) = 0;    /* thread-roles: rolled by cyc_pre on sim */
static int      s_cyc_curr_valid OP_GUARDED_BY(sim) = 0;    /* thread-roles: rolled by cyc_pre on sim */
static int      s_cyc_prev_cam OP_GUARDED_BY(sim) = 0;      /* source = g_Camera fields; thread-roles: rolled by cyc_pre on sim */
static int      s_cyc_curr_cam OP_GUARDED_BY(sim) = 0;      /* thread-roles: rolled by cyc_pre on sim */
static unsigned s_cyc_curr_frame OP_GUARDED_BY(sim) = 0xFFFFFFFFu; /* VBL of the curr capture; thread-roles: rolled by cyc_pre on sim */

/** @brief Capture the sky draw's argument registers and roll the prev/curr matrix pair before the draw clobbers them.
 *
 * Runs as the hook bracket's pre step. On a VBL other than the last capture's,
 * curr rolls to prev, and prev is valid only when the last capture was at
 * most 3 VBLs earlier. The two matrices at a1/a2 are then read into curr,
 * which records whether they are the camera's own view and projection fields.
 * @param cpu The guest CPU state, read only.
 * @param feed Unused.
 * @param verify Unused. */
static void cyc_pre(CPUState* cpu, int feed, int verify) {
    OP_ROLE_ASSERT(sim);
    (void)feed; (void)verify;
    uint32_t a0 = cpu->a0, a1 = cpu->a1, a2 = cpu->a2;
    /* Normalize before the capture: where the sky draw takes no matrix
     * arguments its registers hold whatever the caller left in them, and the
     * sub-tick replay's "a real frame was captured" guard would then test a
     * garbage word: a tick whose stray $a1 happens to be zero reads as "no
     * capture" and the dome is dropped from every present of that interval.
     * The captured triple is the one the extraction uses, on every game. */
    if (!game_layout.cyc.hook_args) a1 = a2 = game_layout.cam.proj_matrix;
    s_cyc_a0 = a0; s_cyc_a1 = a1; s_cyc_a2 = a2;   /* for sub-tick re-extract */
    if (openpete_frame != s_cyc_curr_frame) {
        /* New canonical tick: roll curr->prev. Consecutive = the previous
         * capture was one tick (two VBLs) ago; a longer gap (a load stall,
         * the draw loop pausing) breaks the pair and the re-extract snaps. */
        int consecutive = s_cyc_curr_valid && s_cyc_curr_frame != 0xFFFFFFFFu &&
                          openpete_frame - s_cyc_curr_frame <= 3u;
        memcpy(s_cyc_prev, s_cyc_curr, sizeof s_cyc_prev);
        s_cyc_prev_cam   = s_cyc_curr_cam;
        s_cyc_prev_valid = consecutive;
        s_cyc_curr_frame = openpete_frame;
    }
    ngte_read_matrix(cpu, a1, s_cyc_curr[0]);
    ngte_read_matrix(cpu, a2, s_cyc_curr[1]);
    s_cyc_curr_cam   = !game_layout.cyc.hook_args ||
                       (a1 == game_layout.cam.view_matrix && a2 == game_layout.cam.proj_matrix);
    s_cyc_curr_valid = 1;
}

static hook_bracket_state s_cyc_hb OP_GUARDED_BY(sim);   /* thread-roles: the sky-draw bracket's latch, on sim */
static const hook_bracket_desc k_cyc_bracket = {
    .path = RP_CYCLORAMA, .prof_tag = "cyclorama.srend",
    .verify_enabled = cyc_verify_enabled, .pre = cyc_pre,
    .st = &s_cyc_hb,
};

/** @brief Dispatch hook for the game's sky draw: extract the native dome and optionally verify it.
 *
 * Runs the hook bracket (which calls the real draw body), then extracts the
 * dome with the argument triple cyc_pre stashed, pushes it to the native scene
 * when feeding, and when verifying decodes the prims the draw emitted and
 * compares them from cyc_verify_frame on, dumping the formats once and
 * reporting at most 16 times.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address.
 * @return 0 when the bracket declined to take over, else the bracket's end
 *         result (the body has already run). */
static int on_sky_draw_cyc(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    uint32_t start, end; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_cyc_bracket, &start, &end, &feed, &verify))
        return 0;
    /* cyc_pre stashed the argument triple; the registers are clobbered now. */
    const uint32_t a0 = s_cyc_a0, a1 = s_cyc_a1, a2 = s_cyc_a2;

    cyc_capture_visibility(cpu);
    cyc_vis_census(cpu);

    { OP_PROF_ACCUM_SCOPE("cyclorama.extract");
      cyc_extract(cpu, a0, a1, a2, 0, 0);   /* native scene from gamestate */
      if (feed)
          push_scene(cpu);                  /* always sets the scene bg colour;
                                             * dome tris gated inside on RP_CYCLORAMA */
    }

    if (verify) {
        static int s_dumped OP_GUARDED_BY(sim) = 0;    /* thread-roles: the hook runs on sim */
        static int s_reports OP_GUARDED_BY(sim) = 0;   /* thread-roles: the hook runs on sim */
        int target = (openpete_frame >= cyc_verify_frame() && s_reports < 16);
        if (target && !s_dumped) { hex_dump(cpu, a0, a1, a2, start, end); s_dumped = 1; }
        if (target) {
            capture_gte(cpu, start, end);
            compare_and_report();
            s_reports++;
        }
    }
    return hook_bracket_end(&k_cyc_bracket);  /* took over (body already ran) */
}

/* Sub-tick sky re-extract; documented at the declaration in cyclorama.h. */
void cyclorama_reextract(CPUState* cpu, double alpha, int interp_on) OP_REQUIRES(sim) {
    /* Only replay the sky dome if the canonical frame drew it. In cyclorama-less
     * states (e.g. the black level-name flyby) the saved matrices are stale; the
     * draw-env clear (black) shows through instead. The sky-having transitions
     * (the "ENTERING X" card over a sky) keep RP_CYCLORAMA set, so they're
     * unaffected. The gate also holds the native scene on and the path
     * selected. */
    if (!hook_reextract_ready(RP_CYCLORAMA)) return;
    if (s_cyc_a1 == 0 || s_cyc_a2 == 0 || !s_cyc_curr_valid) return;   /* no real frame captured yet */
    OP_PROF_ACCUM_SCOPE("cyclorama.rex");
    if (s_cyc_curr_cam) {
        cyc_extract(cpu, s_cyc_a0, s_cyc_a1, s_cyc_a2, 0, 0);
    } else {
        int do_lerp = interp_on && s_cyc_prev_valid && !s_cyc_prev_cam;
        if (do_lerp) {
            /* One-tick component delta sanity: a continuous transition dome
             * (camera glide + the spin's 2/4096 per tick) moves a few hundred
             * Q12 units per tick at most. */
            long worst = 0;
            for (int m = 0; m < 2; m++)
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) {
                        long d = (long)s_cyc_curr[m][i][j] - s_cyc_prev[m][i][j];
                        if (d < 0) d = -d;
                        if (d > worst) worst = d;
                    }
            if (worst > 1200) do_lerp = 0;
        }
        int M[2][3][3];
        for (int m = 0; m < 2; m++)
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    if (do_lerp) {
                        double v = (double)s_cyc_prev[m][i][j] +
                                   ((double)s_cyc_curr[m][i][j] - s_cyc_prev[m][i][j]) * alpha;
                        M[m][i][j] = (int)(v >= 0.0 ? v + 0.5 : v - 0.5);
                    } else {
                        M[m][i][j] = s_cyc_curr[m][i][j];
                    }
                }
        cyc_extract_m(cpu, s_cyc_a0, M[0], M[1], 0, 0);
    }
    push_scene(cpu);
}

/* ---- the cross-fade dome (func_8004F000); documented in cyclorama.h ---- */
int cyclorama_native_extract_faded(CPUState* cpu, cyclorama_native_tri* out, int max) OP_REQUIRES(sim) {
    cyc_extract(cpu, 0xFFFFFFFFu, game_layout.cam.view_matrix, game_layout.cam.proj_matrix,
                cpu->read_word(game_layout.fog.far_colour) & 0xFFFFFFu,
                (int)cpu->read_word(game_layout.fog.factor));
    int n = s_native_n < max ? s_native_n : max;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            out[i].sx[k]  = s_native[i].v[k].sx;
            out[i].sy[k]  = s_native[i].v[k].sy;
            out[i].rgb[k] = s_native[i].v[k].rgb;
        }
    return n;
}

/* ---- present-time draw-env clear sync; documented in orchestrator.h ---- */

/* The env a repeat-site row set for the canonical present in flight
 * (isbg, r0, g0, b0); valid from cyclorama_native_repeat_resolve to the
 * next one. */
static uint8_t s_site_env[4] OP_GUARDED_BY(sim);     /* thread-roles: resolved by the commit on sim */
static int     s_site_env_valid OP_GUARDED_BY(sim);  /* thread-roles: resolved by the commit on sim */
/* A kept repeat present (cyclorama_native_repeat_resolve) shows the draw
 * area inside this DISPENV rect, uncleared; valid while s_site_kept is 1. */
static int     s_site_kept_disp[4] OP_GUARDED_BY(sim);  /* thread-roles: resolved by the commit on sim */
static int     s_site_kept OP_GUARDED_BY(sim);          /* thread-roles: resolved by the commit on sim */
/* The env the last canonical present cleared under (isbg, r0, g0, b0), kept
 * for a hold row; valid after the first commit that named one. */
static uint8_t s_presented_env[4] OP_GUARDED_BY(sim);    /* thread-roles: latched by the commit on sim */
static int     s_presented_env_valid OP_GUARDED_BY(sim); /* thread-roles: latched by the commit on sim */

/** @brief Return the PSYQ DRAWENV in the draw buffer g_CurDB points at, read through the sub-tick view.
 *
 * Reads g_CurDB (game_layout.db.cur_ptr), then the 0x1C-byte DRAWENV at the
 * buffer's game_layout.db.drawenv offset: the clip RECT at +0x00, isbg at
 * +0x18, r0/g0/b0 at +0x19..+0x1B.
 * @return The DRAWENV bytes, or NULL when the layout names no pair or
 *         g_CurDB points at neither of its two buffers. */
static const uint8_t* cur_db_drawenv(void) {
    if (!game_layout.db.cur_ptr) return NULL;   /* no draw-buffer pair located */
    const uint8_t* curp = (const uint8_t*)openpete_subtick_host_span(game_layout.db.cur_ptr, 4u);
    if (!curp) return NULL;
    uint32_t curdb;
    memcpy(&curdb, curp, 4);
    if (curdb != game_layout.db.base && curdb != game_layout.db.base + game_layout.db.stride)
        return NULL;   /* not a valid DB* yet */
    /* PSYQ DRAWENV through b0 @+0x1B: 0x1C bytes. */
    return (const uint8_t*)openpete_subtick_host_span(curdb + game_layout.db.drawenv, 0x1Cu);
}

/** @brief Read isbg, r0, g0, b0 of the DRAWENV in the draw buffer g_CurDB points at.
 *
 * @param env Receives isbg, r0, g0, b0 when the function returns 1.
 * @return 1 when the layout names the pair and g_CurDB points at one of its
 *         two buffers, else 0. */
static int cur_db_env(uint8_t env[4]) {
    const uint8_t* e = cur_db_drawenv();
    if (!e) return 0;
    memcpy(env, e + 0x18, 4);
    return 1;
}

/** @brief Report what the draw area the DISPENV in the buffer g_CurDB points at shows holds, read through the sub-tick view.
 *
 * The DISPENV (game_layout.db.dispenv; its display RECT at +0x00) is the
 * one the game puts with the next table; it names the area the last table
 * was drawn into (openpete_native_shown_area_content).
 * @param disp Receives the display rect x, y, w, h; x and y -1 and w, h 0 when no buffer is named.
 * @return 1 content, 0 the fill of an empty table, -1 unknown. */
static int cur_db_shown_content(int disp[4]) {
    disp[0] = disp[1] = -1; disp[2] = disp[3] = 0;
    if (!cur_db_drawenv()) return -1;   /* no valid g_CurDB */
    const uint8_t* curp = (const uint8_t*)openpete_subtick_host_span(game_layout.db.cur_ptr, 4u);
    uint32_t curdb;
    memcpy(&curdb, curp, 4);
    const uint8_t* de = (const uint8_t*)openpete_subtick_host_span(curdb + game_layout.db.dispenv, 8u);
    if (!de) return -1;
    int16_t r[4];
    memcpy(r, de, sizeof r);
    for (int i = 0; i < 4; i++) disp[i] = r[i];
    return openpete_native_shown_area_content(r[0], r[1], r[2], r[3]);
}

/** @brief Report whether OPENPETE_REPEAT_CENSUS is set to a value other than one starting with `0`, read once.
 *
 * @return 1 when every repeat present logs its site, env and verdict. */
static int repeat_census_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_REPEAT_CENSUS"); on = (e && *e && *e != '0'); }
    return on;
}

/** @brief Report whether a row-less repeat present may hold on the shown area's content; OPENPETE_REPEAT_SHOWN=0 disables it (negative control), read once.
 *
 * @return 0 under OPENPETE_REPEAT_SHOWN=0, else 1. */
static int repeat_shown_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_REPEAT_SHOWN"); on = !(e && *e == '0'); }
    return on;
}

/** @brief Return the REPEAT_SITE_* row naming the VSync wait the game is parked in, or REPEAT_SITE_NONE.
 *
 * The wait is the guest return address of the VSync call
 * (openpete_yield_site.ra) the canonical present runs at. */
static int repeat_site_mode(void) {
    const uint32_t ra = openpete_yield_site.ra;
    for (int i = 0; i < GAME_REPEAT_SITE_MAX && game_layout.db.repeat_site[i].yield_ra; i++)
        if (game_layout.db.repeat_site[i].yield_ra == ra) return game_layout.db.repeat_site[i].mode;
    return REPEAT_SITE_NONE;
}

void cyclorama_native_repeat_resolve(void) OP_REQUIRES(sim) {
    s_site_env_valid = 0;
    s_site_kept = 0;
    if (!openpete_native_scene_canon_repeat()) return;
    uint8_t env[4];
    if (!cur_db_env(env)) return;              /* nothing names the env: hold */
    const int mode = repeat_site_mode();
    /* The DRAWENV clears the area the next table draws into, which is not
     * on screen yet: the display shows the area the buffer's DISPENV names,
     * where the last table was drawn. When that table had content the screen
     * keeps its image, uncleared: the present is an empty scene under the
     * env with isbg 0, which shows the retained image of the last rendered
     * present (ui_freeze_retain_commit) without composing the scene again
     * over VRAM the game may have reloaded since. */
    extern unsigned openpete_frame;
    int disp[4];
    const int shown = cur_db_shown_content(disp);
    const int disp_y = disp[1];
    const int kept = mode == REPEAT_SITE_NONE && env[0] && shown == 1 && repeat_shown_on();
    if (repeat_census_on())
        fprintf(stderr, "[repeat] f=%u key=%u ra=%08x mode=%d isbg=%d disp_y=%d shown=%d -> %s\n",
                openpete_frame, openpete_feed_key_now(), openpete_yield_site.ra, mode, env[0], disp_y, shown,
                mode != REPEAT_SITE_NONE ? "row" : kept ? "kept" : env[0] ? "open" : "hold");
    if (mode == REPEAT_SITE_HOLD) {
        /* The screen keeps the presented frame, background included: an env
         * the game puts after the wait clears a later table, not this one. */
        if (s_presented_env_valid) {
            memcpy(s_site_env, s_presented_env, sizeof s_site_env);
            s_site_env_valid = 1;
        }
        return;
    }
    if (kept) {
        env[0] = 0;
        memcpy(s_site_env, env, sizeof s_site_env);
        s_site_env_valid = 1;
        memcpy(s_site_kept_disp, disp, sizeof s_site_kept_disp);
        s_site_kept = 1;
        openpete_native_scene_open_empty();
        return;
    }
    if (mode == REPEAT_SITE_CLEAR_BLACK) {
        env[0] = 1; env[1] = env[2] = env[3] = 0;
    } else if (mode == REPEAT_SITE_CLEAR_DB) {
        env[0] = 1;                            /* the colour stands; isbg is set again before the draw */
    }
    if (mode != REPEAT_SITE_NONE) {
        memcpy(s_site_env, env, sizeof s_site_env);
        s_site_env_valid = 1;
    }
    if (!env[0]) return;                       /* isbg 0: the screen keeps the presented frame */
    openpete_native_scene_open_empty();
}

int cyclorama_native_present_env(uint8_t env[4]) OP_REQUIRES_SHARED(sim) {
    if (openpete_native_drawn_env(env)) return 1;   /* the scene's DrawOTag already ran */
    if (s_site_env_valid) { memcpy(env, s_site_env, sizeof s_site_env); return 1; }
    return cur_db_env(env);
}

/* The background of the last two canonical renders, [0] tick N and [1] tick
 * N-1: the clear the composed scene took and g_Cyclorama.m_BackgroundColor.
 * A repeat present rewrites [0], since it shows the same render. */
typedef struct { uint32_t clear, bg; } cyc_tick_bg_t;
static cyc_tick_bg_t s_tick_bg[2] OP_GUARDED_BY(sim);   /* thread-roles: rolled by the commit on sim */
/* Tick N-1's background word with bit 24 set while a held re-extract runs, else
 * 0. Atomic: the bracket writes it on sim and a moby walk reads it for its fog. */
static OP_ROLE_ATOMIC(uint32_t) s_tick_bg_fog;   /* thread-roles: written on sim, read by the moby walk */

/* Documented at the declaration in orchestrator.h. */
void cyclorama_native_hold_background(int hold) OP_REQUIRES(sim) {
    atomic_store_explicit(&s_tick_bg_fog, hold ? (s_tick_bg[1].bg & 0xFFFFFFu) | (1u << 24) : 0u,
                          memory_order_relaxed);
    if (!hold) return;
    openpete_native_scene_hold_clear(s_tick_bg[1].clear);
    if (cut_census_on())
        fprintf(stderr, "[cut-census vbl=%u] held-background clear=%08x bg=%08x (tick N clear=%08x bg=%08x)\n",
                openpete_frame, s_tick_bg[1].clear, s_tick_bg[1].bg,
                s_tick_bg[0].clear, s_tick_bg[0].bg);
}

/* Documented at the declaration in orchestrator.h. */
uint32_t cyclorama_native_bg_word(CPUState* cpu) {
    const uint32_t held = atomic_load_explicit(&s_tick_bg_fog, memory_order_relaxed);
    return held ? held & 0xFFFFFFu : cpu->read_word(game_layout.cyc.bg_colour);
}

/** @brief Record the background the commit leaves for this render, rolling the tick pair on a new render. */
static void cyc_tick_bg_note(void) OP_REQUIRES(sim) {
    if (!openpete_native_scene_canon_repeat()) s_tick_bg[1] = s_tick_bg[0];
    s_tick_bg[0].clear = openpete_native_clear_color();
    const uint8_t* bg = (const uint8_t*)openpete_psx_to_host_span(game_layout.cyc.bg_colour, 4u);
    if (bg) memcpy(&s_tick_bg[0].bg, bg, 4);
}

/** @brief Sync the native clear and the present draw area from the DRAWENV the presented scene's OT applies. */
static void cyc_env_clear_sync(void) OP_REQUIRES(sim) {
    /* The area the presented scene's OT draws into: the held-fade sum the
     * capture presents is that area's, not the PutDrawEnv latch's. */
    const uint8_t* de = cur_db_drawenv();
    if (s_site_kept) {
        /* A kept repeat draws no table: the present shows the displayed area
         * as it stands, so its fade is that area's sum, not the next one's. */
        openpete_native_note_present_shown_area(s_site_kept_disp[0], s_site_kept_disp[1],
                                                s_site_kept_disp[2], s_site_kept_disp[3]);
    } else if (de) {
        int16_t clip[4];
        memcpy(clip, de, sizeof clip);
        openpete_native_note_present_draw_area(clip[0], clip[1], clip[2], clip[3], de[0x18]);
    } else {
        openpete_native_note_present_draw_area(0, 0, 0, 0, 1);
    }
    uint8_t env[4];
    if (!cyclorama_native_present_env(env)) return;   /* the PutDrawEnv latch stays */
    memcpy(s_presented_env, env, sizeof s_presented_env);
    s_presented_env_valid = 1;
    openpete_native_set_clear_color(env[0], env[1], env[2], env[3]);
}

void cyclorama_native_env_clear_commit(void) OP_REQUIRES(sim) {
    cyc_env_clear_sync();
    cyc_tick_bg_note();
}

/* The hook address is the layout's sky-draw site, bound at boot: a static
 * initializer cannot read another TU's const. */
static feature_dispatch_entry_t cyc_dispatch[] OP_GUARDED_BY(sim) = {   /* thread-roles: bound in the boot window, which holds sim */
    { 0u, on_sky_draw_cyc, 0u },
    { 0u, NULL, 0u },
};

/** @brief Bind the sky-draw dispatch entry to the layout's hook address.
 *
 * @param opts Unused. */
static void cyc_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    cyc_dispatch[0].addr = game_layout.hooks.sky_draw;
}

const feature_module_t cyclorama_module = {
    .name           = "cyclorama_native",
    .apply_options  = cyc_apply_options,
    .dispatch_table = cyc_dispatch,
};
