// games/spyro-1/renderer/portal.c - native re-derivation of the PORTAL
// skybox previews (the destination-level sky shown inside homeworld portal
// arches), plus its falsify-before-ship capture/dump harness.
//
// Sub-tick spine: portal previews ride the high-rate re-extract like every
// other feeder, by CAPTURE-AND-REPLAY. (1) On each canonical RenderPortals
// the leaf hooks record every drawn portal's decision (owning portal,
// flat/dome kind, resolved colour, fog, clip rect) into s_pcap[];
// portal_reextract (in the spine's feeder list, after cyclorama) replays them
// on every intermediate present, so the previews are re-added each sub-tick.
// (2) The dome rotation matrix is not read from the live GTE on the reextract
// (it is stale on the host fiber). A dome a leaf drew with the GTE matrix
// replays the componentwise lerp prev->curr of the shared dome matrix
// snapshots (s_dome_prev, s_dome_curr) by the spine's sub-tick alpha, as the
// spine interpolates the camera matrix (lerp_matrix); the camera rotation and
// the portal's own spin (D_80075858/D_800758FC) are both baked into the
// snapshots, so both interpolate with no extrapolation. A derived dome (the
// --no-lod forced dome, a revealed portal's dome) rederives the matrix under
// the staged camera with the spin angles lerped between the last two
// canonical ticks (portal_replay_derived_matrix). The base clip rect is
// rebuilt per replay under the interpolated camera by the rule the canonical
// rect came from (portal_replay_rect); outline depth comes from
// project_world, which reads the interpolated camera live. See
// portal_reextract() below.
//
// Background. The main level sky is func_8004EBA8 (handled by
// cyclorama.c). The PORTAL previews are a separate pipeline driven by
// func_80050BD0 (RenderPortals): for each visible portal it projects the
// portal's own destination skybox dome with an animated rotation matrix, sets
// a per-portal screen CLIP RECT (D_8007AA00..0C), and emits the dome prims via
// one of three handwritten leaf renderers -
//   func_8004FEA0  close range (< 0x3001), full colour
//   func_80050240  medium range, distance colour-blended (D_800757D4 target)
//   func_8004F4BC  secondary pass
// - then finally calls func_8004EBA8 for the main sky.
//
// This module re-derives the portal dome in float from gamestate (faithful
// to the renderer's "3D-from-gamestate, never replay GPU packets" design,
// and widescreen-ready). Its capture/dump harness hooks the leaf renderers +
// RenderPortals and snapshots the
// inputs (skybox struct, the two matrices, clip rect, depth) and the GTE prims
// each leaf actually emitted to the polybuf (ground truth), so the float
// extraction can be measured against real output. Gated by OPENPETE_PORTAL_DUMP
// (zero overhead off). Determinism: the real leaf renderer runs exactly once
// (behaviour-identical fall-through) and the dump only reads. The one PSX RAM
// store is the D_CLIPRECT stage of feed_revealed_portals and portal_reextract,
// written only when the sub-tick view cannot stage it and restored before
// return.
//
// Format notes: cyclorama.h warns m_Skybox is "strange storage", so trust the
// dump, not the field names.

#include "openpete_knobs.h"   /* knob_env_epoch_stale */
#include "feature_registry.h"
#include "psx_runtime_internal.h"
#include "openpete_subtick_view.h"
#include "openpete_native_render.h"
#include "openpete_bugrec_subtick.h"
#include "openpete_fiber.h"        /* openpete_feed_key_now */
#include "openpete_interp.h"
#include "openpete_interp_objs.h"   /* g_openpete_tick_period_ns */
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"   /* hook_reextract_ready_held */
#include "renderer/cut_guard.h"      /* cut_held_sets_t: the held cut's tick N-1 set */
#include "reextract.h"   /* portal_reextract export */
#include "portal.h"         /* cross-feeder exports (terrain OT-conformance clamp) */
#include "renderer/portal_plane.h"   /* openpete_portal_plane_margin (shared) */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "renderer/terrain.h"       /* terrain_native_flush_deferred */
#include "renderer/orchestrator.h"  /* canonical-render epoch */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro-1/gen/spyro1_symbols.gen.h"
#include "game_layout.h"
#include "openpete_pace_log.h"   /* OP_PACE_SPLIT_SCOPE - the guest call's split */
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE - fiber-hook cost split */
#include "openpete_thread_roles.h"

extern void     func_8004FEA0(CPUState* cpu);  /* portal sky: close, full colour */
extern void     func_80050240(CPUState* cpu);  /* portal sky: medium, blended    */
extern void     func_8004F4BC(CPUState* cpu);  /* portal sky: secondary pass     */
extern void     func_80050BE0(CPUState* cpu);  /* RenderPortals (the driver)     */
extern unsigned openpete_frame;


/** @brief Blends an 0x00BBGGRR colour toward the fog target by t, as the GTE DPCS depth cue does.
 *
 * The blended-medium portal's cross-dissolve (func_80050240) uses t =
 * IR0 / 4096 and fog = D_800757D4. A t above 1 clamps to 1. Each channel
 * moves by (int)(delta * t + 0.5): nearest for a rising channel, up to 1
 * smaller in magnitude than nearest for a falling one (the cast truncates
 * toward zero).
 * @param rgb The colour, 0x00BBGGRR.
 * @param fog The fog target, 0x00BBGGRR.
 * @param t The blend factor; t <= 0 returns the colour unchanged (the un-fogged dome or flat).
 * @return The blended colour, 0x00BBGGRR. */
static uint32_t blend_to_fog(uint32_t rgb, uint32_t fog, double t) {
    if (t <= 0.0) return rgb & 0xFFFFFF;
    if (t > 1.0) t = 1.0;
    int r0 =  rgb        & 0xFF, g0 = (rgb  >> 8) & 0xFF, b0 = (rgb  >> 16) & 0xFF;
    int rf =  fog        & 0xFF, gf = (fog  >> 8) & 0xFF, bf = (fog  >> 16) & 0xFF;
    int r = r0 + (int)((rf - r0) * t + 0.5);
    int g = g0 + (int)((gf - g0) * t + 0.5);
    int b = b0 + (int)((bf - b0) * t + 0.5);
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

/* ---- dump gating ---------------------------------------------------- */
/** @brief Reports whether OPENPETE_PORTAL_DUMP enables the portal leaf dump.
 *
 * The dump prints up to 512 leaf calls once openpete_frame reaches
 * OPENPETE_PORTAL_FRAME (default 1750). The variable is read once and
 * cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int portal_dump_enabled(void) {
    static int s = -1;
    if (s < 0) { const char* v = getenv("OPENPETE_PORTAL_DUMP"); s = (v && v[0] && v[0] != '0') ? 1 : 0; }
    return s;
}
/** @brief Returns the openpete_frame at which the portal dump starts.
 *
 * Parsed from OPENPETE_PORTAL_FRAME with atoi, 1750 when unset. The
 * variable is read once and cached.
 * @return The first openpete_frame value the dump prints at. */
static unsigned portal_dump_frame(void) {
    static unsigned f = 0; static int init = 0;
    if (!init) { const char* v = getenv("OPENPETE_PORTAL_FRAME"); f = v ? (unsigned)atoi(v) : 1750u; init = 1; }
    return f;
}

static double project_world(CPUState* cpu, const ngte_view* v, double wx, double wy, double wz, double* out_sx, double* out_sy);
/* The projection view of the pass in progress (leaf hook, RenderPortals
 * reveal, sub-tick replay), built at each entry. */
static ngte_view s_view;

typedef struct { float x, y, r, g, b; } pgv;   /* gouraud screen vertex (clipping) */
typedef struct { float x, y; double vz; } ppt; /* projected portal-outline point  */
/* The view Z an outline is clipped at before projecting: project_world's
 * screen divide clamps at 1, so every projected point lies beyond it. */
#define PORTAL_NEAR_VZ 1.0
/* Capacity of a projected outline: one plane cut adds at most one vertex per
 * two crossings, so the 16-point outline clips to at most 24. */
#define PORTAL_POLY_MAX 32
/* The 4:3 frame's bottom edge, RenderPortals' clip-rect y clamp (0xF0). */
#define PORTAL_SCREEN_H 240

/* OPENPETE_PORTAL_CLIP_CENSUS: how the flat and dome feeders projected their
 * portal's outline, per pass (0 = canonical leaf and reveal feeds, 1 =
 * sub-tick replay): every point in front of the near plane, cut by it, or no
 * outline in an extended view, where the feeder fills the clip rect
 * instead. Printed at exit with the canonical RenderPortals count. */
#define PORTAL_CLIP_FRONT    0
#define PORTAL_CLIP_CLIPPED  1
#define PORTAL_CLIP_FALLBACK 2
static unsigned s_clip_tally[2][3];
static unsigned s_clip_last[3];    /* canonical tallies of the last RenderPortals run */
static unsigned s_clip_cur[3];     /* canonical tallies of the RenderPortals run in progress */
static unsigned s_clip_presents;   /* canonical RenderPortals runs with the native feed */
static int      s_clip_replay;     /* 1 while portal_reextract replays the capture */

/** @brief Prints the OPENPETE_PORTAL_CLIP_CENSUS tallies to stderr. */
static void portal_clip_census_print(void) {
    OP_ROLE_ASSERT(sim);
    for (int k = 0; k < 2; k++)
        fprintf(stderr, "portal: clip census %s: front=%u near-clipped=%u ext-rect-fallback=%u "
                "(canonical presents=%u)\n", k ? "replay" : "canonical",
                s_clip_tally[k][PORTAL_CLIP_FRONT], s_clip_tally[k][PORTAL_CLIP_CLIPPED],
                s_clip_tally[k][PORTAL_CLIP_FALLBACK], s_clip_presents);
    fprintf(stderr, "portal: clip census last canonical present: front=%u near-clipped=%u "
            "ext-rect-fallback=%u\n", s_clip_last[PORTAL_CLIP_FRONT],
            s_clip_last[PORTAL_CLIP_CLIPPED], s_clip_last[PORTAL_CLIP_FALLBACK]);
}

/** @brief Counts one feeder outline verdict for OPENPETE_PORTAL_CLIP_CENSUS.
 *
 * Inert unless the variable is set and not "0"; the first call reads it and
 * registers the exit print.
 * @param verdict PORTAL_CLIP_FRONT, PORTAL_CLIP_CLIPPED or PORTAL_CLIP_FALLBACK. */
static void portal_clip_note(int verdict) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("OPENPETE_PORTAL_CLIP_CENSUS");
        on = (e && *e && *e != '0');
        if (on) atexit(portal_clip_census_print);
    }
    if (!on) return;
    s_clip_tally[s_clip_replay ? 1 : 0][verdict]++;
    if (!s_clip_replay) s_clip_cur[verdict]++;
}
/* Owning portal of a leaf: skybox-pointer match (dome/close), else screen-centre
 * match (far/medium flat). Returns the Portal* or 0. */
static uint32_t owning_portal_ptr(CPUState* cpu, const ngte_view* v, uint32_t a0, double cx, double cy);
static int    portal_outline_screen(CPUState* cpu, const ngte_view* v, uint32_t p, ppt* poly, int maxn);

/* Per-portal opening-polygon correction, in world units, along the opening
 * plane's own horizontal/vertical axes.
 *
 * Some arches have their outline authored slightly off the stone it belongs to
 * - offset to one side, or simply smaller than the hole. The original never
 * showed it: its projection was coarse enough to absorb a few units. At this
 * renderer's precision the shortfall is visible as a band of through-hole
 * terrain along one jamb that no depth policy can close, because the preview
 * genuinely does not reach that far.
 *
 * Keyed by DESTINATION levelId, which identifies a portal uniquely within its
 * homeworld. OPENPETE_PORTAL_NUDGE="id:dh,dv[,sh[,sv]];..." overrides at
 * runtime for tuning; +dh is right along the opening, +dv is up. Three fields
 * translate only, four scale uniformly, five scale per-axis. */
typedef struct { int levelId; double dh, dv, sh, sv; } portal_nudge_t;
/* Measured, not eyeballed. Each opening was compared against its own arch in
 * the opening plane: the inner rim is the band that gets culled, so the OUTER
 * boundary of the culled rim is exactly where surviving stone begins - the
 * inner edge the preview has to meet. The correction is the transform taking
 * the authored outline's bbox onto that boundary's bbox, and each entry was
 * confirmed by re-measuring with the correction applied (residual <2 world
 * units on openings 1900-3200 units across, i.e. under 0.1%).
 *
 * Only five arches need one. Every other portal in the game already agrees
 * with its stone to within ~10 units and is deliberately left untouched.
 *
 * NOTE the dh sign is per-portal, not global: the horizontal axis is
 * normal x vertical, and the authored winding decides which way the normal
 * points, so two arches that look identical can want opposite dh. Never carry
 * a sign over from a neighbouring portal - measure it.
 *
 * The dragon-head arches (14, 62/63/64) are absent ON PURPOSE. Their geometry
 * animates, so a measurement taken at one frame describes a mouth caught
 * mid-open, not the opening. */
static const portal_nudge_t k_portal_nudge[] = {
    /* Alignment: the opening does not sit on its arch. */
    { 11,  48.8, -3.8, 1.0425, 1.0000 },  /* artisans -> stone_hill: opening sits right of its arch */
    { 41,  11.2,  4.4, 1.0803, 1.0585 },  /* beast_makers -> terrace_village: outline ~8% undersized */
    { 42, -16.5, -5.0, 1.0820, 1.0629 },  /* beast_makers -> misty_bog */
    { 44,  -3.0, -3.2, 1.0770, 1.0641 },  /* beast_makers -> metalhead */
    { 45,  -2.0, -4.4, 1.0824, 1.0585 },  /* beast_makers -> wild_flight */
    /* Sill: the bottom edge grazes the floor. See the note below. */
    { 13,   0.0, -3.0, 1.0000, 1.0016 },  /* artisans -> town_square */
    { 21,   0.0, -5.0, 1.0000, 1.0046 },  /* peace_keepers -> dry_canyon */
    { 25,   0.0, -4.0, 1.0000, 1.0038 },  /* peace_keepers -> night_flight */
    { 33,   0.0, -4.0, 1.0000, 1.0043 },  /* magic_crafters -> wizard_peak */
    { 52,   0.0, -4.0, 1.0000, 1.0021 },  /* dream_weavers -> lofty_castle */
    { 54,   0.0, -4.0, 1.0000, 1.0018 },  /* dream_weavers -> jacques */
};
/* The sill entries fix a different defect from the alignment ones, and are
 * paired dv/sv on purpose: dv drops the opening's BOTTOM edge, and sv is
 * exactly the compensation that leaves the TOP where it was, so nothing opens
 * up against the arch above.
 *
 * Their bottom edge sat within a couple of units of the floor that meets the
 * sill, and the floor is triangulated, so along the edge the preview was
 * alternately in front of and behind it - the visible junction flipped between
 * the polygon's straight edge and the floor's contour, breaking into a
 * staircase. dry_canyon crosses outright: its bottom edge runs from v=0.0 to
 * v=-7.4 while the floor's top runs from -1.7 to -5.4. Dropping the edge a few
 * units puts it under the floor for the whole span, so the floor occludes it
 * cleanly and the junction stops flickering between two answers.
 *
 * Minimums measured from renders (town_square clears at -2, dry_canyon at -4,
 * the rest at -4); the values here carry a unit of margin where known. This
 * defect could NOT be settled in the opening plane the way the alignment ones
 * were - the floor stands in FRONT of the plane, so which of the two wins is a
 * projective question, and an in-plane test calls every one of these fine.
 *
 * Finding them needs care. A diff between the corrected and uncorrected render
 * looks identical whether a sliver vanished or the destination sky merely slid
 * a pixel, and a sharp horizon in the sky art produces exactly the same
 * "many changed pixels, few rows" signature a sliver does. magic_crafters ->
 * alpine_ridge(31) and gnorc_gnexus -> gnorc_cove(61) were flagged that way
 * and are NOT defects - the flag was their own horizon moving. Confirm the
 * artifact by eye at the rows the diff fires on before adding a row here.
 * (34, 51, 55 also flagged and were not confirmed either way; left alone.) */
/* OPENPETE_PORTAL_NUDGE parsed into rows; at most PORTAL_NUDGE_ENV_MAX. */
#define PORTAL_NUDGE_ENV_MAX 32
typedef struct { int levelId; double dh, dv, sh, sv; } portal_nudge_row_t;
typedef struct { int n; portal_nudge_row_t row[PORTAL_NUDGE_ENV_MAX]; } portal_nudge_env_t;

/** @brief OPENPETE_PORTAL_NUDGE parsed into rows, re-parsed only when the registry env epoch changes.
 *
 * The spelling is "id:dh,dv[,sh[,sv]];...": three fields translate only,
 * four scale uniformly, five scale per axis; an entry with fewer than three
 * fields is skipped.
 * @return The parsed rows. */
static const portal_nudge_env_t* portal_nudge_env(void) {
    static portal_nudge_env_t E;
    static unsigned seen = 0;
    if (!knob_env_epoch_stale(&seen)) return &E;
    E.n = 0;
    const char* q = getenv("OPENPETE_PORTAL_NUDGE");
    while (q && *q && E.n < PORTAL_NUDGE_ENV_MAX) {
        int id = 0; double a = 0.0, b = 0.0, c = 1.0, d = 1.0;
        const int got = sscanf(q, "%d:%lf,%lf,%lf,%lf", &id, &a, &b, &c, &d);
        if (got >= 3)
            E.row[E.n++] = (portal_nudge_row_t){ id, a, b, got >= 4 ? c : 1.0,
                                                 got >= 5 ? d : (got >= 4 ? c : 1.0) };
        while (*q && *q != ';') q++;
        if (*q == ';') q++;
    }
    return &E;
}

/** @brief Looks up a portal's opening-polygon correction by destination levelId.
 *
 * OPENPETE_PORTAL_NUDGE="id:dh,dv[,sh[,sv]];..." is searched first: three
 * fields translate only, four scale uniformly, five scale per axis. Then
 * k_portal_nudge. With no entry the outputs are the identity (0, 0, 1, 1).
 * @param levelId The portal's destination levelId.
 * @param dh Receives the horizontal translation, world units, + right along the opening.
 * @param dv Receives the vertical translation, world units, + up.
 * @param sh Receives the horizontal scale.
 * @param sv Receives the vertical scale.
 * @return 1 when an environment entry matched or the table entry is not the identity, else 0. */
static int portal_nudge_for(int levelId, double* dh, double* dv,
                            double* sh, double* sv) {
    *dh = 0.0; *dv = 0.0; *sh = 1.0; *sv = 1.0;
    const portal_nudge_env_t* E = portal_nudge_env();
    for (int i = 0; i < E->n; i++)
        if (E->row[i].levelId == levelId) {
            *dh = E->row[i].dh; *dv = E->row[i].dv; *sh = E->row[i].sh; *sv = E->row[i].sv;
            return 1;
        }
    for (size_t i = 0; i < sizeof k_portal_nudge / sizeof k_portal_nudge[0]; i++)
        if (k_portal_nudge[i].levelId == levelId) {
            *dh = k_portal_nudge[i].dh; *dv = k_portal_nudge[i].dv;
            *sh = k_portal_nudge[i].sh; *sv = k_portal_nudge[i].sv;
            return (*dh != 0.0 || *dv != 0.0 || *sh != 1.0 || *sv != 1.0);
        }
    return 0;
}

/** @brief Reads a portal's outline m_Points (+0x20, 12 bytes each) in world space, optionally corrected.
 *
 * The single site that reads m_Points, so the projected polygon, the plane
 * fit and the spill depth agree on where the opening is. The correction
 * (portal_nudge_for, keyed on the destination levelId at +0x1C) serves
 * rendering only; code classifying the mesh (which faces lie in the
 * opening plane) reads the authored outline, so a corrected preview
 * position cannot change geometry classification. The correction
 * translates by dh, dv along the in-plane horizontal and vertical axes
 * (vertical = world +Z projected into the plane) and scales by sh, sv
 * about the horizontal centre and the bottom edge, so the opening's foot
 * stays on the sill. The out-of-plane component is carried through, so a
 * corrected outline spans the same plane as the authored one. An outline
 * lying flat (no in-plane vertical) or degenerate in its first three
 * points is returned uncorrected.
 * @param cpu The guest CPU state.
 * @param p The Portal* guest address.
 * @param W Receives the outline points, world units.
 * @param nudge 1 applies the per-portal correction, 0 reads the authored outline.
 * @return The point count, 0 when the count at +0x04 is outside [3, 16]. */
static int portal_outline_world(CPUState* cpu, uint32_t p, double W[16][3], int nudge) {
    const int npts = (int32_t)cpu->read_word(p + 0x04);
    if (npts < 3 || npts > 16) return 0;
    for (int k = 0; k < npts; k++) {
        W[k][0] = (double)(int32_t)cpu->read_word(p + 0x20 + k*12);
        W[k][1] = (double)(int32_t)cpu->read_word(p + 0x24 + k*12);
        W[k][2] = (double)(int32_t)cpu->read_word(p + 0x28 + k*12);
    }
    if (!nudge) return npts;
    double dh = 0.0, dv = 0.0, sh = 1.0, sv = 1.0;
    if (!portal_nudge_for((int32_t)cpu->read_word(p + 0x1C), &dh, &dv, &sh, &sv)) return npts;
    /* in-plane axes: vertical = world +Z projected into the plane */
    const double e1[3] = { W[1][0]-W[0][0], W[1][1]-W[0][1], W[1][2]-W[0][2] };
    const double e2[3] = { W[2][0]-W[0][0], W[2][1]-W[0][1], W[2][2]-W[0][2] };
    double nx = e1[1]*e2[2]-e1[2]*e2[1], ny = e1[2]*e2[0]-e1[0]*e2[2], nz = e1[0]*e2[1]-e1[1]*e2[0];
    double nl = sqrt(nx*nx + ny*ny + nz*nz);
    if (nl < 1e-9) return npts;
    nx /= nl; ny /= nl; nz /= nl;
    double ux = 0.0, uy = 0.0, uz = 1.0;
    const double dot = uz*nz;
    ux -= dot*nx; uy -= dot*ny; uz -= dot*nz;
    double ul = sqrt(ux*ux + uy*uy + uz*uz);
    if (ul < 1e-9) return npts;                     /* opening lies flat */
    ux /= ul; uy /= ul; uz /= ul;
    const double hx = ny*uz - nz*uy, hy = nz*ux - nx*uz, hz = nx*uy - ny*ux;
    /* Decompose about the opening's own origin, correct, recompose.
     *
     * Translation alone cannot fix an outline that is simply SMALLER than the
     * arch it belongs to (stone_hill: no horizontal offset covers it), so a
     * scale is needed too - anchored at HORIZONTAL CENTRE, VERTICAL BOTTOM.
     * Not the centroid: the opening's base sits on the sill, and that junction
     * has to stay a straight line on the authored edge, so a vertical scale
     * must grow the arch upward and leave its foot exactly where it is.
     * The out-of-plane component is carried through untouched, so a slightly
     * non-planar authored outline is not flattened. */
    double ox = W[0][0], oy = W[0][1], oz = W[0][2];
    double hlo = 1e30, hhi = -1e30, vlo = 1e30;
    for (int k = 0; k < npts; k++) {
        const double r[3] = { W[k][0]-ox, W[k][1]-oy, W[k][2]-oz };
        const double rh = r[0]*hx + r[1]*hy + r[2]*hz;
        const double rv = r[0]*ux + r[1]*uy + r[2]*uz;
        if (rh < hlo) hlo = rh;
        if (rh > hhi) hhi = rh;
        if (rv < vlo) vlo = rv;
    }
    const double ah = 0.5 * (hlo + hhi);            /* horizontal centre */
    const double av = vlo;                          /* bottom edge */
    for (int k = 0; k < npts; k++) {
        const double r[3] = { W[k][0]-ox, W[k][1]-oy, W[k][2]-oz };
        const double rh = r[0]*hx + r[1]*hy + r[2]*hz;
        const double rv = r[0]*ux + r[1]*uy + r[2]*uz;
        const double rn = r[0]*nx + r[1]*ny + r[2]*nz;
        const double gh = ah + (rh - ah) * sh + dh;
        const double gv = av + (rv - av) * sv + dv;
        W[k][0] = ox + gh*hx + gv*ux + rn*nx;
        W[k][1] = oy + gh*hy + gv*uy + rn*ny;
        W[k][2] = oz + gh*hz + gv*uz + rn*nz;
    }
    return npts;
}

/** @brief Prints the RenderPortals state and every portal record to stderr.
 *
 * Prints g_PortalCount, the spin yaw and pitch, then for up to six portals:
 * the skybox, destination levelId, outline point count (flagged OVER-CAP
 * outside [3, 16]), background colour, and the centroid's screen position
 * and view Z. For an outline of 3 to 16 points it also prints the world
 * centroid, plane normal and radius, and, when the centroid is in front of
 * the camera, each projected point under s_view.
 * @param cpu The guest CPU state. */
static void dump_portal_table(CPUState* cpu) {
    uint32_t n = cpu->read_word(G_PORTALCOUNT);
    fprintf(stderr, "[portal-dump f=%u] RenderPortals: g_PortalCount=%u yaw=%d pitch=%d\n",
            openpete_frame, n, (int32_t)cpu->read_word(D_PORTAL_YAW),
            (int32_t)cpu->read_word(D_PORTAL_PITCH));
    if (n > 6) n = 6;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = cpu->read_word(G_PORTALS + i * 4);
        if (!p) { fprintf(stderr, "  portal[%u] = NULL\n", i); continue; }
        uint32_t skybox = cpu->read_word(p + 0x00);
        int levelId     = (int32_t)cpu->read_word(p + 0x1C);
        /* m_PointCount: the outline polygon's vertex count. Every outline reader
         * here caps at 16 (ppt poly[16]) and BAILS above it, so a count >16 costs
         * the plane fit, the two-region spill split and the widescreen un-clamp. */
        int npts        = (int32_t)cpu->read_word(p + 0x04);
        /* Centroid of the outline points (p+0x20 + 12*k), projected as the game
         * does - confirms which portals are on-screen + their view-Z. */
        double sx, sy, vz = project_world(cpu, &s_view, (int32_t)cpu->read_word(p + 0x20),
                                          (int32_t)cpu->read_word(p + 0x24),
                                          (int32_t)cpu->read_word(p + 0x28), &sx, &sy);
        fprintf(stderr, "  portal[%u]@%08X skybox=%08X levelId=%d npts=%d%s bg=%06X -> screen(%.0f,%.0f) viewZ=%.0f\n",
                i, p, skybox, levelId, npts, (npts > 16 || npts < 3) ? " OVER-CAP" : "",
                cpu->read_word(skybox + 0x18) & 0xFFFFFF, sx, sy, vz);
        /* World-space geometry of the opening: centroid + plane normal + radius.
         * Camera poses for the per-portal verification tour are derived from
         * these, so every arch is framed the same way instead of hand-flown. */
        if (npts >= 3 && npts <= 16) {
            double cxw = 0, cyw = 0, czw = 0;
            for (int k = 0; k < npts; k++) {
                cxw += (int32_t)cpu->read_word(p + 0x20 + k*12);
                cyw += (int32_t)cpu->read_word(p + 0x24 + k*12);
                czw += (int32_t)cpu->read_word(p + 0x28 + k*12);
            }
            cxw /= npts; cyw /= npts; czw /= npts;
            const double ax = (int32_t)cpu->read_word(p + 0x20) - cxw;
            const double ay = (int32_t)cpu->read_word(p + 0x24) - cyw;
            const double az = (int32_t)cpu->read_word(p + 0x28) - czw;
            const int k2 = npts / 2;
            const double bx = (int32_t)cpu->read_word(p + 0x20 + k2*12) - cxw;
            const double by = (int32_t)cpu->read_word(p + 0x24 + k2*12) - cyw;
            const double bz = (int32_t)cpu->read_word(p + 0x28 + k2*12) - czw;
            double nx2 = ay*bz - az*by, ny2 = az*bx - ax*bz, nz2 = ax*by - ay*bx;
            const double nl = sqrt(nx2*nx2 + ny2*ny2 + nz2*nz2);
            if (nl > 1e-9) { nx2 /= nl; ny2 /= nl; nz2 /= nl; }
            double rad = 0.0;
            for (int k = 0; k < npts; k++) {
                const double dx = (int32_t)cpu->read_word(p + 0x20 + k*12) - cxw;
                const double dy = (int32_t)cpu->read_word(p + 0x24 + k*12) - cyw;
                const double dz = (int32_t)cpu->read_word(p + 0x28 + k*12) - czw;
                const double r = sqrt(dx*dx + dy*dy + dz*dz);
                if (r > rad) rad = r;
            }
            fprintf(stderr, "    world c=(%.0f,%.0f,%.0f) n=(%.4f,%.4f,%.4f) rad=%.0f\n",
                    cxw, cyw, czw, nx2, ny2, nz2, rad);
        }
        if (npts >= 3 && npts <= 16 && vz > 1.0) {
            fprintf(stderr, "    outline:");
            for (int k = 0; k < npts; k++) {
                double ox, oy, ovz = project_world(cpu, &s_view,
                        (int32_t)cpu->read_word(p + 0x20 + k*12),
                        (int32_t)cpu->read_word(p + 0x24 + k*12),
                        (int32_t)cpu->read_word(p + 0x28 + k*12), &ox, &oy);
                fprintf(stderr, " [%d](%.1f,%.1f)vz=%.0f", k, ox, oy, ovz);
            }
            fprintf(stderr, "\n");
        }
    }
}

/** @brief Resolves the flat-fill colour a portal leaf carries in a0.
 *
 * The FLAT leaf passes a pointer whose [0] is the colour; both dome-family
 * leaves (DOME func_8004F4BC and BLENDED func_80050240) pass the skybox
 * struct, whose m_BackgroundColor is at +0x18. The caller states which,
 * because the leaf hook knows the leaf: the word's magnitude cannot
 * discriminate, since the animated flat colour drops below 0x10000 at its
 * cycle trough (blue and green reach zero). The canonical feed and the
 * sub-tick capture both resolve through this function.
 * @param cpu The guest CPU state.
 * @param a0 The leaf's a0.
 * @param a0_is_skybox 1 when a0 is a skybox struct, 0 when it points at the colour.
 * @return The raw 0x00BBGGRR colour before fog, 0 when a0 is outside [0x80010000, 0x80200000). */
static uint32_t portal_flat_raw_rgb(CPUState* cpu, uint32_t a0, int a0_is_skybox) {
    if (a0 < 0x80010000u || a0 >= 0x80200000u) return 0;
    return cpu->read_word(a0 + (a0_is_skybox ? 0x18u : 0u)) & 0xFFFFFF;
}

/** @brief Decodes the GTE rotation registers gte_ctrl[0..4] (Q12) into a 3x3 matrix.
 *
 * Read in a dome leaf, the registers hold the dome matrix RenderPortals left
 * there: the camera rotation composed with the portal's animated spin
 * (D_80075858 yaw, D_800758FC pitch). The capture path snapshots it as
 * s_dome_curr each canonical tick; the dump path compares it with the
 * derived dome matrix.
 * @param cpu The guest CPU state.
 * @param M Receives the matrix, Q12. */
static void read_gte_rotmatrix(CPUState* cpu, int M[3][3]) {
    ngte_unpack_matrix(cpu->gte_ctrl[0], cpu->gte_ctrl[1], cpu->gte_ctrl[2],
                       cpu->gte_ctrl[3], cpu->gte_ctrl[4], M);
}

/* ---- sub-tick capture-and-replay ------------------------------------------
 * The canonical RenderPortals leaf hooks record each DRAWN portal's high-level
 * decision here (the transient clip rect / colour / GTE matrix are live only
 * during the leaf call); portal_reextract replays them on every
 * intermediate present, re-projecting the geometry against the interpolated
 * camera. One entry per leaf call = one flat quad (+ optional dome). */
typedef struct {
    uint32_t portal;        /* owning Portal* (outline re-projection)            */
    uint32_t flat_a0;       /* the leaf's a0 (skybox-link check; may be stale)    */
    uint32_t flat_rgb;      /* resolved raw flat colour (a0 is stale on replay)   */
    uint32_t skybox;        /* skybox ptr for the dome walk (is_dome only)        */
    int      is_dome;       /* leaf emitted a gradient dome (kind != FLAT)        */
    int      kind;          /* PORTAL_FLAT/DOME/BLENDED - is_dome collapses the
                             * last two, but a material selector keys on the
                             * full trichotomy, so keep it for replay           */
    uint32_t ot_b;          /* captured D_PORTAL_DEPTH (the .s OT bucket)          */
    uint32_t fog_rgb;       /* blended-medium DPCS fog target (0 if none)         */
    double   fog_t;         /* blended-medium fog lerp coeff (0 if none)          */
    int      clip[4];       /* base clip rect, D_CLIPRECT words >> 16: min y, max y,
                             * then the x pair. FLAT, DOME and CAPTURED hold the
                             * game's layout, max x then min x; OUTLINE holds
                             * min x then max x. The feeders sort each pair or
                             * use its midpoint; portal_rect_rule_for compares
                             * it element-wise with portal_game_rect's output,
                             * which is in the game's layout.                    */
    int      rect_rule;     /* how the replay rebuilds clip[] (PORTAL_RECT_*)     */
    int      derived_dome;  /* the dome drew with derive_dome_matrix (the --no-lod
                             * forced dome, a revealed portal's dome), not the
                             * matrix a dome leaf left in the GTE                 */
    int      far_fog;       /* fog_rgb / fog_t are RenderPortals' far-leaf term
                             * (portal_far_fog_t), re-evaluated for `portal`
                             * under each present's camera                       */
    int      src;           /* BUGREC_SUB_PORTAL_LEAF or _REVEAL: which feed made it */
    unsigned frame;         /* openpete_frame of the RenderPortals run that made it */
} portal_cap_t;
/* The rule a replay rebuilds a capture's base clip rect by, under the camera
 * of the present being built. */
#define PORTAL_RECT_CAPTURED 0  /* no rule reproduces it: the captured rect      */
#define PORTAL_RECT_FLAT     1  /* RenderPortals' outline bbox (flat leaf)       */
#define PORTAL_RECT_DOME     2  /* the same bbox, points pushed 2 px outward     */
#define PORTAL_RECT_OUTLINE  3  /* revealed portal: the float outline bbox       */
#define PORTAL_CAP_MAX 16
static portal_cap_t  s_pcap_store[2][PORTAL_CAP_MAX];
static portal_cap_t* s_pcap = s_pcap_store[0];      /* the last RenderPortals' captures */
static portal_cap_t* s_pcap_old = s_pcap_store[1];  /* the set that run retired */
static int          s_pcap_n = 0;     /* entries captured this canonical frame    */
static int          s_pcap_old_n = 0; /* entries of the retired set               */
static int          s_pcap_valid = 0; /* a canonical RenderPortals has populated it*/
static unsigned     s_pcap_frame = 0; /* openpete_frame of the populating RenderPortals */
static uint32_t     s_pcap_epoch = 0; /* canonical-render epoch of the populating RenderPortals */
static int          s_pcap_pick = -1; /* the set a held re-extract shows (cut_held_pick_t), -1 outside a hold */

/** @brief Exchange the current capture set with the retired one, with their counts. */
static void pcap_sets_swap(void) {
    portal_cap_t* t = s_pcap; s_pcap = s_pcap_old; s_pcap_old = t;
    const int n = s_pcap_n; s_pcap_n = s_pcap_old_n; s_pcap_old_n = n;
}

static cut_held_sets_t s_sets = { pcap_sets_swap, &s_pcap_n, "portal", { 0u, 0u }, 0u };

/* Staleness gate: on_80050BE0 calls
 * render_path_mark_drawn(RP_PORTAL) while the canonical frame renders; the
 * shared render_paths commit latches it into the canon-drawn mask once per
 * canonical present, BEFORE the sub-tick reextract reads it via
 * render_path_canon_drawn(RP_PORTAL). When RenderPortals stops (load
 * transition) the bit drops next present and the reextract stops replaying
 * the stale capture. */

/* Shared dome rotation matrix, snapshotted prev+curr per canonical tick. RenderPortals
 * builds ONE dome matrix per frame (byte-identical across all portals - the GTEmat
 * the leaves load), so a single pair suffices. The reextract LERPS prev->curr by the
 * spine's sub-tick alpha (componentwise, like the camera's lerp_matrix) - both the
 * camera rotation AND the portal's own spin are baked in, so both interpolate
 * smoothly with no extrapolation.
 * Rolled curr->prev at the top of each canonical RenderPortals; curr refilled when a
 * dome leaf fires. prev is valid only if the immediately-preceding canonical frame
 * also drew a dome (consecutive), else the reextract snaps to curr. */
static int          s_dome_prev[3][3], s_dome_curr[3][3];
static int          s_dome_prev_valid = 0, s_dome_curr_valid = 0;
/* The portal spin angles (D_PORTAL_YAW, D_PORTAL_PITCH) after each canonical
 * RenderPortals, prev and curr: the inputs a derived dome matrix
 * (derive_dome_matrix) takes besides the camera. */
static double       s_spin_prev[2], s_spin_curr[2];
static int          s_spin_prev_valid = 0, s_spin_curr_valid = 0;
/* g_Camera's rotation angles (+0x4C roll, +0x4E pitch, +0x50 heading) after
 * each canonical RenderPortals, prev and curr. The sub-tick camera bracket
 * lerps the camera matrices and position but stages the euler angles of tick
 * N unchanged, so a derived dome on a sub-tick present takes its camera
 * angles from this pair. Valid together with the spin pair. */
static double       s_camang_prev[3], s_camang_curr[3];

/** @brief Projects a world point in float as func_80017B48 projects the portal outline.
 *
 * func_80017B48 uses the g_Camera+0x00 rotation with TR = 0 and a permuted,
 * negated vertex: GTE V = (-(wy - cy), -(wz - cz), wx - cx). The per-point
 * right shift it applies cancels in the MAC1/MAC3 and MAC2/MAC3 ratios, so
 * the float path applies none. The screen divide clamps view Z to at least
 * 1. Coordinates are doubles because a corrected outline lies between the
 * authored integer points; truncating it moves the opening by up to one
 * world unit.
 * @param cpu Unused.
 * @param v The view supplying the camera position, rotation, OFX, OFY and H.
 * @param wx The world X.
 * @param wy The world Y.
 * @param wz The world Z.
 * @param out_sx Receives the screen X; may be NULL.
 * @param out_sy Receives the screen Y; may be NULL.
 * @return The view-space Z (MAC3), unclamped. */
static double project_world(CPUState* cpu, const ngte_view* v, double wx, double wy, double wz,
                            double* out_sx, double* out_sy) {
    (void)cpu;
    double vx = -(wy - v->cam_ram[1]);    /* GTE VX = -(world.y - cam.y) */
    double vy = -(wz - v->cam_ram[2]);    /* GTE VY = -(world.z - cam.z) */
    double vz =  (wx - v->cam_ram[0]);    /* GTE VZ =  (world.x - cam.x) */
    double mac1, mac2, mac3;
    ngte_mac(v->cam_M, vx, vy, vz, &mac1, &mac2, &mac3);
    const double ofx = v->ofx, ofy = v->ofy, H = v->H_eff;
    double z = (mac3 > 1.0) ? mac3 : 1.0;
    if (out_sx) *out_sx = ofx + mac1 * H / z;
    if (out_sy) *out_sy = ofy + mac2 * H / z;
    return mac3;
}

/* ---- RenderPortals' clip rect, integer mirror -------------------------------
 * RenderPortals (func_80050BD0) projects every outline point m_Points[k]
 * (p+0x20+12k) through func_80017B48 and stores the clamped bbox of the SXY
 * results in D_8007AA00 before each leaf call (0x80051814: the flat leaf
 * func_8004FEA0; 0x80051EA8: the dome leaves func_8004F4BC / func_80050240,
 * after moving every point 2 px away from the points' mean on each axis).
 * func_80017B48 feeds RTPS the camera-relative point shifted right by a
 * per-portal amount derived from the distance to m_Points[0]
 * (0x80050F1C-0x80050FC8). Reproducing that arithmetic under the camera of
 * the present being built gives the rect the game would have produced there;
 * at the captured camera it equals the captured rect. */

/** @brief func_80016D08: 31 minus the GTE LZCR of `a`; 0 for 0.
 *
 * The LZCR counts the leading bits equal to the sign bit. */
static int portal_game_log2(int32_t a) {
    if (a == 0) return 0;
    const uint32_t u = (a < 0) ? ~(uint32_t)a : (uint32_t)a;
    const int lz = u ? __builtin_clz(u) : 32;
    return 31 - lz;
}

/** @brief VecMagnitude(v, 1) (math.s): the magnitude of `v`.
 *
 * SQR of the int16 components, normalised by the even LZCR count, looked up in
 * the square-root table D_SQRT_TABLE and scaled back. */
static int32_t portal_game_magnitude(CPUState* cpu, const int32_t v[3]) {
    int64_t sum = 0;
    for (int k = 0; k < 3; k++) { const int64_t ir = (int16_t)v[k]; sum += ir * ir; }
    const int32_t s = (int32_t)(uint32_t)sum;
    if (s == 0) return 0;
    const int lz = 31 - portal_game_log2(s);
    const int a1 = lz & ~1;
    const int a0 = (31 - a1) >> 1;
    const int32_t n = (a1 >= 24) ? (int32_t)((uint32_t)s << (a1 - 24)) : (s >> (24 - a1));
    const int idx = n - 0x40;
    if (idx < 0 || idx > 0xBF) return 0;
    const int32_t t = (int16_t)cpu->read_half(D_SQRT_TABLE + 2u * (uint32_t)idx);
    return (int32_t)(((uint32_t)t << a0) >> 12);
}

/** @brief RenderPortals' distance from the camera at `cam` to portal `p`'s first outline point.
 *
 * VecSub(m_Points[0] (p+0x20), g_Camera+0x28), then VecMagnitude, the vector
 * first shifted right by log2(L1)/2 (func_80016D08) and the magnitude shifted
 * back when the L1 norm reaches 2^15 (0x80050F1C-0x80050FA8). The result is
 * the d that sets the RTPS shift and the far-leaf fog term.
 * @param cpu The guest CPU state.
 * @param cam The camera position in world units, truncated to int32.
 * @param p The Portal pointer.
 * @return The integer distance. */
static int32_t portal_game_distance(CPUState* cpu, const double cam[3], uint32_t p) {
    int32_t d[3];
    for (int k = 0; k < 3; k++)
        d[k] = (int32_t)cpu->read_word(p + 0x20u + 4u * (uint32_t)k) - (int32_t)cam[k];
    uint32_t l1 = 0;
    for (int k = 0; k < 3; k++) l1 += (uint32_t)(d[k] < 0 ? -d[k] : d[k]);
    const int bits = portal_game_log2((int32_t)l1);
    int32_t mag;
    if (bits >= 15) {
        const int s = bits >> 1;
        for (int k = 0; k < 3; k++) d[k] >>= s;
        mag = (int32_t)((uint32_t)portal_game_magnitude(cpu, d) << s);
    } else {
        mag = portal_game_magnitude(cpu, d);
    }
    return mag;
}

/** @brief The right shift RenderPortals applies to a portal's camera-relative points before RTPS.
 *
 * The shift is max(0, log2(d) - 13), d the portal_game_distance of the view's
 * integer camera (0x80050FAC-0x80050FC4). */
static int portal_game_shift(CPUState* cpu, const ngte_view* v, uint32_t p) {
    const int sh = portal_game_log2(portal_game_distance(cpu, v->cam_ram, p)) - 13;
    return sh < 0 ? 0 : sh;
}

/** @brief The SXY func_80017B48 leaves for each outline point of portal `p` under the view's camera.
 *
 * Mirrors the 0x8005105C loop. Returns the point count, 0 when the portal has
 * no usable outline. */
static int portal_game_project(CPUState* cpu, const ngte_view* v, uint32_t p, int sx[16], int sy[16]) {
    const int n = (int32_t)cpu->read_word(p + 0x04);
    if (n < 1 || n > 16) return 0;
    const int sh = portal_game_shift(cpu, v, p);
    const int tr0[3] = { 0, 0, 0 };
    const int32_t cx = (int32_t)v->cam_ram[0], cy = (int32_t)v->cam_ram[1], cz = (int32_t)v->cam_ram[2];
    for (int k = 0; k < n; k++) {
        const uint32_t a = p + 0x20u + 12u * (uint32_t)k;
        const int32_t vz = ((int32_t)cpu->read_word(a + 0) - cx) >> sh;   /* VZ0 */
        const int32_t vx = (cy - (int32_t)cpu->read_word(a + 4)) >> sh;   /* VX0 */
        const int32_t vy = (cz - (int32_t)cpu->read_word(a + 8)) >> sh;   /* VY0 */
        ngte_rtps_out o;
        ngte_rtps_int(v->cam_M, tr0, (int16_t)vx, (int16_t)vy, (int16_t)vz,
                      v->ofx, v->ofy, (int)v->H_canon, &o);
        sx[k] = o.sx; sy[k] = o.sy;
    }
    return n;
}

/** @brief The dome leaves' push (0x800518CC-0x800519E4).
 *
 * Each coordinate moves 2 away from the truncated mean of its axis, unchanged
 * when equal. */
static void portal_game_push_out(int* c, int n) {
    int32_t sum = 0;
    for (int k = 0; k < n; k++) sum += c[k];
    const int32_t m = sum / n;
    for (int k = 0; k < n; k++) c[k] += (m < c[k]) ? 2 : (c[k] < m) ? -2 : 0;
}

/** @brief RenderPortals' clip rect for portal `p` under the view's camera, in the capture's layout.
 *
 * The layout is D_8007AA00 words >> 16: min y, max y, max x, min x. `dome`
 * selects the dome leaves' rect, whose points are pushed out first and whose
 * min-x search starts at 0x200 instead of 0x13F8. Returns 0 when the portal has
 * no usable outline. */
static int portal_game_rect(CPUState* cpu, const ngte_view* v, uint32_t p, int dome, int rc[4]) {
    int sx[16], sy[16];
    const int n = portal_game_project(cpu, v, p, sx, sy);
    if (!n) return 0;
    if (dome) { portal_game_push_out(sx, n); portal_game_push_out(sy, n); }
    int x0 = dome ? 0x200 : 0x13F8, x1 = 0, y0 = 0xF0, y1 = 0;
    for (int k = 0; k < n; k++) {
        if (sx[k] > x1) x1 = sx[k];
        if (sx[k] < x0) x0 = sx[k];
        if (sy[k] > y1) y1 = sy[k];
        if (sy[k] < y0) y0 = sy[k];
    }
    rc[0] = y0 < 0 ? 0 : y0;
    rc[1] = y1 > 0xF0 ? 0xF0 : y1;
    rc[2] = x1 > 0x200 ? 0x200 : x1;
    rc[3] = x0 < 0 ? 0 : x0;
    return 1;
}

/** @brief Returns the largest view Z among a portal's outline points.
 *
 * The arch's pointed top curves back from the opening's centre, so a sky at
 * the centre depth overdraws the top of the frame; at the farthest outline
 * depth the stone occludes it into the arch shape, and it stays nearer than
 * the through-hole background. The dome path and the flat quad both use it
 * through portal_viewz.
 * @param cpu The guest CPU state.
 * @param v The view to project under.
 * @param p The Portal* guest address.
 * @param nudge 1 for the corrected outline (rendering), 0 for the authored one (the arch's own geometry).
 * @return The largest view Z, -1 when the outline is unusable or every point has view Z below -1. */
static double portal_outline_maxvz(CPUState* cpu, const ngte_view* v, uint32_t p, int nudge) {
    double W[16][3];
    const int npts = portal_outline_world(cpu, p, W, nudge);
    if (!npts) return -1.0;
    double maxvz = -1.0;
    for (int k = 0; k < npts; k++) {
        const double vz = project_world(cpu, v, W[k][0], W[k][1], W[k][2], 0, 0);
        if (vz > maxvz) maxvz = vz;
    }
    return maxvz;
}

/** @brief The portal whose skybox a leaf's a0 names: m_Skybox (p+0x00) equal to a0 or to a0 - 0x10.
 *
 * The dome leaves pass m_Skybox, the close flat leaf m_Skybox + 0x10. At
 * most six portals of g_Portals are searched.
 * @param cpu The guest CPU state.
 * @param a0 The leaf's a0.
 * @return The Portal* guest address, 0 when no skybox matches. */
static uint32_t portal_by_skybox_link(CPUState* cpu, uint32_t a0) {
    uint32_t n = cpu->read_word(G_PORTALCOUNT); if (n > 6) n = 6;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = cpu->read_word(G_PORTALS + i * 4); if (!p) continue;
        uint32_t sb = cpu->read_word(p + 0x00);
        if (sb && (sb == a0 || sb == a0 - 0x10)) return p;
    }
    return 0;
}

/** @brief Finds the portal a leaf call draws for.
 *
 * The close and dome leaves pass the owning portal's skybox directly
 * (func_8004F4BC: a0 = m_Skybox; the close func_8004FEA0: a0 = m_Skybox +
 * 0x10), matched exactly against each portal's +0x00. The screen-centre
 * match cannot serve them: a large, screen-clamped portal's clip-rect
 * centre drifts onto another portal. Far and medium flat leaves carry a
 * stack or global colour pointer with no skybox link, so they take the
 * portal whose outline centroid projects, under the canonical view,
 * nearest the clip-rect centre. At most six portals are searched; an
 * outline count outside [1, 16] is read as 5.
 * @param cpu The guest CPU state.
 * @param v The running view; the centroid match uses its canonical copy.
 * @param a0 The leaf's a0.
 * @param cx The clip rect's centre X.
 * @param cy The clip rect's centre Y.
 * @return The Portal* guest address, 0 when none matches. */
static uint32_t owning_portal_ptr(CPUState* cpu, const ngte_view* v, uint32_t a0, double cx, double cy) {
    const uint32_t linked = portal_by_skybox_link(cpu, a0);   /* skybox-pointer match */
    if (linked) return linked;
    uint32_t n = cpu->read_word(G_PORTALCOUNT); if (n > 6) n = 6;
    /* The clip-rect centre is a screen coordinate the game produced at its own
     * H, so the centroid is matched against it through the canonical view. */
    ngte_view vc; ngte_view_canon(v, &vc);
    double best_d = 1e30; uint32_t best_p = 0;          /* screen-centre match */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = cpu->read_word(G_PORTALS + i * 4); if (!p) continue;
        int npts = (int32_t)cpu->read_word(p + 0x04); if (npts < 1 || npts > 16) npts = 5;
        double ax = 0, ay = 0, az = 0;
        for (int k = 0; k < npts; k++) {
            ax += (int32_t)cpu->read_word(p + 0x20 + k*12);
            ay += (int32_t)cpu->read_word(p + 0x24 + k*12);
            az += (int32_t)cpu->read_word(p + 0x28 + k*12);
        }
        ax /= npts; ay /= npts; az /= npts;
        double sx, sy, vz = project_world(cpu, &vc, ax, ay, az, &sx, &sy);
        if (vz <= 1.0) continue;
        double dx = sx - cx, dy = sy - cy, d = dx*dx + dy*dy;
        if (d < best_d) { best_d = d; best_p = p; }
    }
    return best_p;
}

/** @brief Returns the constant sky view Z of the portal a leaf draws for.
 *
 * The farthest point of the corrected outline (portal_outline_maxvz), after
 * resolving the portal through owning_portal_ptr. The feeders use it where
 * the per-pixel plane depth is unavailable and for the ring depth mode.
 * @param cpu The guest CPU state.
 * @param v The view to project under.
 * @param a0 The leaf's a0.
 * @param cx The clip rect's centre X.
 * @param cy The clip rect's centre Y.
 * @return The view Z, -1 when no portal matches or its outline is unusable. */
static double portal_viewz(CPUState* cpu, const ngte_view* v, uint32_t a0, double cx, double cy) {
    uint32_t p = owning_portal_ptr(cpu, v, a0, cx, cy);
    return p ? portal_outline_maxvz(cpu, v, p, 1) : -1.0;   /* render depth */
}

/** @brief Rotates a world point into the portal outline's view space, as project_world does.
 *
 * @param v The view supplying the camera position and rotation.
 * @param w The world point.
 * @param out Receives MAC1, MAC2 and MAC3 (view Z). */
static void portal_view_point(const ngte_view* v, const double w[3], double out[3]) {
    ngte_mac(v->cam_M, -(w[1] - v->cam_ram[1]), -(w[2] - v->cam_ram[2]), (w[0] - v->cam_ram[0]),
             &out[0], &out[1], &out[2]);
}

/** @brief Projects one view-space point to screen with project_world's divide.
 *
 * @param v The view supplying OFX, OFY and H.
 * @param q The view-space point (MAC1, MAC2, MAC3).
 * @return The screen point and its view Z. */
static ppt portal_view_project(const ngte_view* v, const double q[3]) {
    const double z = (q[2] > 1.0) ? q[2] : 1.0;
    ppt o;
    o.x = (float)(v->ofx + q[0] * v->H_eff / z);
    o.y = (float)(v->ofy + q[1] * v->H_eff / z);
    o.vz = q[2];
    return o;
}

/** @brief Clips a view-space polygon at the near plane PORTAL_NEAR_VZ and projects it.
 *
 * Sutherland-Hodgman against view Z > PORTAL_NEAR_VZ: a crossing point is
 * interpolated linearly in view space and placed on the plane. A polygon
 * wholly in front projects vertex for vertex, so its points equal
 * project_world's. The cut edge lies on the near plane, where the
 * projection is H times the view-space offset, off screen for any polygon
 * whose part nearer than the plane is under a world unit across.
 * @param v The view.
 * @param q The view-space polygon.
 * @param n Its vertex count.
 * @param out Receives the projected polygon.
 * @param maxn The capacity of out.
 * @param clipped Receives 1 when a vertex lay at or behind the plane, else 0; may be NULL.
 * @return The projected vertex count; 0 when fewer than 3 remain or maxn would be exceeded. */
static int portal_clip_near(const ngte_view* v, const double (*q)[3], int n,
                            ppt* out, int maxn, int* clipped) {
    int m = 0, cut = 0;
    for (int i = 0; i < n; i++) {
        const double* a = q[i];
        const double* b = q[(i + 1) % n];
        const int ain = a[2] > PORTAL_NEAR_VZ, bin = b[2] > PORTAL_NEAR_VZ;
        if (ain) {
            if (m >= maxn) return 0;
            out[m++] = portal_view_project(v, a);
        } else {
            cut = 1;
        }
        if (ain != bin) {
            const double t = (PORTAL_NEAR_VZ - a[2]) / (b[2] - a[2]);
            const double c[3] = { a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), PORTAL_NEAR_VZ };
            if (m >= maxn) return 0;
            out[m++] = portal_view_project(v, c);
        }
    }
    if (clipped) *clipped = cut;
    return m >= 3 ? m : 0;
}

/** @brief Projects a portal's outline (m_Points), clipped at the near plane, to screen with per-point view Z.
 *
 * The one projection of the outline every preview consumer uses: the flat
 * fill, the dome, the stencil aperture, the widescreen reveal and the
 * sub-tick replay rect. An outline with points behind the camera keeps its
 * part in front (portal_clip_near), so a close opening at a wide view still
 * has its polygon and plane; only an outline wholly behind the camera has
 * none.
 * @param cpu The guest CPU state.
 * @param v The view to project under.
 * @param p The Portal* guest address.
 * @param nudge 1 for the corrected outline (rendering), 0 for the authored one.
 * @param poly Receives the projected points.
 * @param maxn The capacity of poly.
 * @param clipped Receives 1 when the near plane cut the outline, else 0; may be NULL.
 * @return The point count, at least 3; 0 when the outline is unusable, wholly behind the near plane, or exceeds maxn. */
static int portal_outline_clipped(CPUState* cpu, const ngte_view* v, uint32_t p, int nudge,
                                  ppt* poly, int maxn, int* clipped) {
    double W[16][3], q[16][3];
    if (clipped) *clipped = 0;
    const int npts = portal_outline_world(cpu, p, W, nudge);
    if (!npts) return 0;
    for (int k = 0; k < npts; k++) portal_view_point(v, W[k], q[k]);
    return portal_clip_near(v, (const double (*)[3])q, npts, poly, maxn, clipped);
}

/** @brief Projects a portal's authored outline, before any per-portal correction, to screen with per-point view Z.
 *
 * A corrected outline reaches past the authored one into the arch; that
 * band is not depth-treated like the opening interior, so its inner edge
 * is taken from this projection.
 * @param cpu The guest CPU state.
 * @param v The view to project under.
 * @param p The Portal* guest address.
 * @param poly Receives the projected points.
 * @param maxn The capacity of poly.
 * @return The point count, 0 when the outline is unusable, exceeds maxn, or has a point with view Z at most 1. */
static int portal_outline_screen_raw(CPUState* cpu, const ngte_view* v, uint32_t p, ppt* poly, int maxn) {
    int cut = 0;
    const int n = portal_outline_clipped(cpu, v, p, 0, poly, maxn, &cut);
    return cut ? 0 : n;
}

/** @brief Projects a portal's corrected outline (m_Points) to screen with per-point view Z, all points in front.
 *
 * Reads the outline through portal_outline_world with the per-portal
 * correction applied. The terrain plane provider and the dump take this
 * form; the preview feeders take the near-clipped one
 * (portal_outline_clipped).
 * @param cpu The guest CPU state.
 * @param v The view to project under.
 * @param p The Portal* guest address.
 * @param poly Receives the projected points.
 * @param maxn The capacity of poly.
 * @return The point count, at least 3, when every point has view Z above 1;
 *         0 when the outline is unusable, exceeds maxn, or has a point at or
 *         behind that. */
static int portal_outline_screen(CPUState* cpu, const ngte_view* v, uint32_t p, ppt* poly, int maxn) {
    int cut = 0;
    const int n = portal_outline_clipped(cpu, v, p, 1, poly, maxn, &cut);
    return cut ? 0 : n;
}

/** @brief The clip rect of a projected outline: its bounding box, outward to whole pixels.
 *
 * The layout is the OUTLINE capture's: min y, max y, min x, max x. A rect
 * stored in a D_CLIPRECT word (the reveal's and the replay's base rect)
 * holds int16: a near-clipped outline, or one whose box leaves that range,
 * reaches far off screen, so its stored box is clamped to the view, x to the
 * Hor+ bounds and y to [0, PORTAL_SCREEN_H]. The dome's face clip takes the
 * box unclamped (saturated at +-1e9): a gouraud face cut at the screen edge
 * gets screen-linear colours along the cut, which the perspective-correct
 * raster then spreads across the visible part of the face.
 * @param v The view supplying the screen-X bounds.
 * @param poly The projected outline.
 * @param pn The point count, at least 1.
 * @param clipped 1 when the near plane cut the outline.
 * @param stored 1 for a rect stored in a D_CLIPRECT word, 0 for the dome's face clip.
 * @param r Receives the rect. */
static void portal_outline_rect(const ngte_view* v, const ppt* poly, int pn, int clipped, int stored,
                                int r[4]) {
    float xlo = poly[0].x, xhi = poly[0].x, ylo = poly[0].y, yhi = poly[0].y;
    for (int k = 1; k < pn; k++) {
        xlo = fminf(xlo, poly[k].x); xhi = fmaxf(xhi, poly[k].x);
        ylo = fminf(ylo, poly[k].y); yhi = fmaxf(yhi, poly[k].y);
    }
    const float lim = stored ? 32767.0f : 1e9f;
    const int to_view = stored && (clipped || xlo < -lim || xhi > lim || ylo < -lim || yhi > lim);
    const float bx0 = to_view ? floorf(v->xlo) : -lim, bx1 = to_view ? ceilf(v->xhi) : lim;
    const float by0 = to_view ? 0.0f : -lim, by1 = to_view ? (float)PORTAL_SCREEN_H : lim;
    r[0] = (int)floorf(fminf(fmaxf(ylo, by0), by1)); r[1] = (int)ceilf(fminf(fmaxf(yhi, by0), by1));
    r[2] = (int)floorf(fminf(fmaxf(xlo, bx0), bx1)); r[3] = (int)ceilf(fminf(fmaxf(xhi, bx0), bx1));
}

/** @brief Maps RenderPortals' clip rect onto an extended view for a feeder with no outline in front of the camera.
 *
 * Counts the fallback for OPENPETE_PORTAL_CLIP_CENSUS in an extended view.
 * The game projects the rect at the GTE's H and clamps it to the 4:3 frame
 * [0, 512] x [0, PORTAL_SCREEN_H]. In an extended view an edge at that clamp
 * moves to the view's bound, and an edge inside it is rescaled about OFX,
 * OFY by the --fov scale to the H the feeders project with. A canonical
 * view keeps the rect.
 * @param v The view.
 * @param x0 The rect's minimum X, updated.
 * @param y0 The rect's minimum Y, updated.
 * @param x1 The rect's maximum X, updated.
 * @param y1 The rect's maximum Y, updated. */
static void portal_fallback_rect(const ngte_view* v, int* x0, int* y0, int* x1, int* y1) {
    if (!v->extended) return;
    portal_clip_note(PORTAL_CLIP_FALLBACK);
    *x0 = (*x0 <= 0)   ? (int)floorf(v->xlo) : (int)floor(v->ofx + (*x0 - v->ofx) * v->s);
    *x1 = (*x1 >= 512) ? (int)ceilf(v->xhi)  : (int)ceil (v->ofx + (*x1 - v->ofx) * v->s);
    *y0 = (*y0 <= 0) ? 0 : (int)floor(v->ofy + (*y0 - v->ofy) * v->s);
    *y1 = (*y1 >= PORTAL_SCREEN_H) ? PORTAL_SCREEN_H : (int)ceil(v->ofy + (*y1 - v->ofy) * v->s);
}

/** @brief Fits 1/vz = a*x + b*y + c through three points of a planar projected outline.
 *
 * 1/w is linear in screen space for a plane, so the fit is the portal
 * window's depth field. Samples the points at indices 0, n/3 and 2n/3.
 * @param poly The projected outline.
 * @param n The point count.
 * @param a Receives the x coefficient.
 * @param b Receives the y coefficient.
 * @param c Receives the constant.
 * @return 1 on success, 0 when the samples coincide or are collinear. */
static int fit_invz(const ppt* poly, int n, double* a, double* b, double* c) {
    int i0 = 0, i1 = n / 3, i2 = (2 * n) / 3;
    if (i1 == i0) i1 = 1;
    if (i2 == i1) i2 = n - 1;
    if (i2 == i0) return 0;
    double x0 = poly[i0].x, y0 = poly[i0].y, w0 = 1.0 / poly[i0].vz;
    double x1 = poly[i1].x, y1 = poly[i1].y, w1 = 1.0 / poly[i1].vz;
    double x2 = poly[i2].x, y2 = poly[i2].y, w2 = 1.0 / poly[i2].vz;
    double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (det > -1e-6 && det < 1e-6) return 0;            /* collinear */
    *a = ((w1 - w0) * (y2 - y0) - (w2 - w0) * (y1 - y0)) / det;
    *b = ((x1 - x0) * (w2 - w0) - (x2 - x0) * (w1 - w0)) / det;
    *c = w0 - *a * x0 - *b * y0;
    return 1;
}

/** @brief Computes the screen-space 1/vz plane of a portal's opening from its world outline and the camera.
 *
 * Needs no projected points, so it stays valid when an outline point is
 * behind the camera and fit_invz cannot run; a constant sky depth there
 * lets the floor cover the near side of a close, steeply viewed opening.
 * Three corrected outline points (indices 0, n/3, 2n/3) are rotated with
 * project_world's convention, V = (-dy, -dz, dx), MAC = M V. For the
 * view-space plane n.P = k, dividing by vz gives
 * 1/vz = (n_x (sx - ofx) / H + n_y (sy - ofy) / H + n_z) / k, the
 * a x + b y + c form plane_depth consumes.
 * @param cpu The guest CPU state.
 * @param v The view supplying the camera, OFX, OFY and H.
 * @param p The Portal* guest address.
 * @param pa Receives the x coefficient.
 * @param pb Receives the y coefficient.
 * @param pc Receives the constant.
 * @return 1 on success, 0 when the outline is unusable or collinear, the camera lies within 1e-3 of the plane, or H is below 1. */
static int portal_plane_invz(CPUState* cpu, const ngte_view* v, uint32_t p,
                             double* pa, double* pb, double* pc) {
    int npts = (int32_t)cpu->read_word(p + 0x04);
    if (npts < 3 || npts > 16) return 0;
    const int (*M)[3] = v->cam_M;
    const double cx = v->cam_ram[0], cy = v->cam_ram[1], cz = v->cam_ram[2];
    int idx[3] = { 0, npts / 3, (2 * npts) / 3 };        /* spread trio (fit_invz) */
    if (idx[1] == idx[0]) idx[1] = 1;
    if (idx[2] == idx[1]) idx[2] = npts - 1;
    if (idx[2] == idx[0]) return 0;
    double Wn[16][3];
    if (!portal_outline_world(cpu, p, Wn, 1)) return 0;
    double V[3][3];
    for (int k = 0; k < 3; k++) {
        ngte_mac(M, -(Wn[idx[k]][1] - cy), -(Wn[idx[k]][2] - cz), (Wn[idx[k]][0] - cx),
                 &V[k][0], &V[k][1], &V[k][2]);
    }
    double e1x = V[1][0]-V[0][0], e1y = V[1][1]-V[0][1], e1z = V[1][2]-V[0][2];
    double e2x = V[2][0]-V[0][0], e2y = V[2][1]-V[0][1], e2z = V[2][2]-V[0][2];
    double nx = e1y*e2z - e1z*e2y;
    double ny = e1z*e2x - e1x*e2z;
    double nz = e1x*e2y - e1y*e2x;
    double nn = sqrt(nx*nx + ny*ny + nz*nz);
    if (nn < 1e-6) return 0;                             /* collinear outline */
    double k0 = nx*V[0][0] + ny*V[0][1] + nz*V[0][2];    /* n·P = k0, cam at origin */
    if (k0 > -1e-3*nn && k0 < 1e-3*nn) return 0;         /* camera ~on the plane */
    const double ofx = v->ofx, ofy = v->ofy, H = v->H_eff;
    if (H < 1.0) return 0;
    *pa = nx / (H * k0);
    *pb = ny / (H * k0);
    *pc = (nz - nx*ofx/H - ny*ofy/H) / k0;
    return 1;
}

/* The portal cyclorama (the gouraud dome AND the flat far/medium preview) sits on
 * the portal OPENING PLANE. The original confines it to the arch purely by OT
 * painter's order - RenderPortals paints the sky OVER everything seen through the
 * opening and UNDER the arch stone. The native path reproduces that with the
 * opening plane's own depth, pulled nearer only by the plane-cover margin
 * (portal_cyclorama_depth); no forward bias is added. A forward bias would win
 * the depth test against the arch's own geometry at an angle and show the sky
 * past the stone sides. The ~1px inner-frame edge, a decorative band at the
 * opening plane, z-fights with the sky and can flicker. */

/* The portal sky's tie-break margin, in WORLD view-Z units, pushed behind the
 * opening plane on the ring, spill and correction-band constructions
 * (portal_sky_eps_vz). A fixed DEPTH-BUFFER offset would be
 * distance-PROPORTIONAL in world units through d = vz/(vz+4096): ~10 units at
 * vz 1000, ~55 at vz 8000, ~450 at vz 30000, which at mid/far range swallows
 * the arch's own stone wherever its surface runs within it of the opening plane
 * (the jamb/rim seen at a grazing camera angle). A WORLD-unit margin keeps the
 * same tie-break at every distance, and the sliver where it can beat real stone
 * stays O(24 world units), sub-pixel at any range and above raster
 * interpolation error. 24 rather than 16 because the decorative inner band sits
 * a few units in FRONT of the opening plane. */
#define PORTAL_SKY_EPS_VZ_DEFAULT 24.0
/** @brief Returns how far behind the opening plane the aperture's correction band is marked, in world units.
 *
 * The band lies between the authored and the corrected outline. The value
 * only needs to exceed the depth quantum plus any authoring slop in the
 * stone it overlaps; it never applies to the opening interior. Default 24,
 * overridden by OPENPETE_PORTAL_BAND_BACK, read once.
 * @return The push-back in world units. */
static double portal_band_back_vz(void) {
    static int init = 0; static double v = 24.0;
    if (!init) {
        const char* e = getenv("OPENPETE_PORTAL_BAND_BACK");
        if (e && *e) v = atof(e);
        init = 1;
    }
    return v;
}
/** @brief Returns the push-back that places the ring and spill sky constructions behind the opening plane, in world units.
 *
 * Default PORTAL_SKY_EPS_VZ_DEFAULT, overridden by OPENPETE_PORTAL_SKY_EPS
 * and read once; 0 applies no push. The margin is tunable so it can be
 * recalibrated against the depth encoding without a rebuild. A negative
 * value moves the constructions in front of the plane.
 * @return The push-back in world units. */
static double portal_sky_eps_vz(void) {
    static int init = 0;
    static double v = PORTAL_SKY_EPS_VZ_DEFAULT;
    if (!init) {
        const char* e = getenv("OPENPETE_PORTAL_SKY_EPS");
        if (e && *e) v = atof(e);
        init = 1;
    }
    /* A negative value moves the constructions in front of the plane, where
     * they win contests against coincident geometry but also against receding
     * jamb stone. Zero leaves them coplanar with whatever else the arch has at
     * the opening plane. A positive push back makes every piece of the frame
     * win cleanly. */
    return v;
}
/* The .s inserts the sky at bucket B = (Σ outline SZ / npts) >> 7, bumped
 * +64 slots iff B >= 256 (RenderPortals, block_800515C4) - the same +0x40
 * constant the terrain walk adds to every LP face's OT index. Terrain
 * carries its authored slot offsets as world-unit pushes on its test depth,
 * so a far portal's sky must carry its matching 64-slot push (8192 world
 * units) or it beats the near-plane band of terrain the original repaints
 * over it (arch jambs and shoulders read as preview sky). Bumped buckets
 * are identifiable from the bucket value alone: any pre-bump B >= 256 lands
 * at >= 320, so 256..319 is unreachable. Applied to EVERY sky construction
 * of the feed (interior, spill, mask ring, correction band, backstop) so
 * the whole skirt shifts together; zero for near portals, and forced zero
 * mid-entry where the near cover must win everything.
 * OPENPETE_PORTAL_SKY_OTPUSH=0 = un-pushed plane A/B. */
static double s_sky_push_vz = 0.0;
/** @brief Whether the far-portal sky OT push is on; a live row, re-read when the registry env epoch changes. */
static int sky_otpush_enabled(void) {
    static int on = 1;
    static unsigned seen = 0;
    if (knob_env_epoch_stale(&seen)) {
        const char* e = getenv("OPENPETE_PORTAL_SKY_OTPUSH");
        on = !(e && *e && atoi(e) == 0);
    }
    return on;
}
/** @brief Returns the world-unit push a sky construction takes for an OT depth.
 *
 * 128 * 64 (the 64-slot bucket bump, in world units) when ot_vz is at least
 * 128 * 320 and OPENPETE_PORTAL_SKY_OTPUSH is not 0, else 0.
 * @param ot_vz 128 times the mirrored RenderPortals OT bucket.
 * @return The push in world units. */
static double sky_ot_push_for(double ot_vz) {
    return (ot_vz >= 128.0 * 320.0 && sky_otpush_enabled()) ? 128.0 * 64.0 : 0.0;
}
/** @brief Converts a portal plane's view Z to the scene depth the mask and preview colours test at.
 *
 * Subtracts openpete_portal_plane_margin() from vz, floors the result at 1,
 * and maps it through openpete_worldvz_full and openpete_native_scene_viewz.
 * @param vz The plane's world-scale view Z.
 * @return The scene depth. */
static float portal_cyclorama_depth(double vz) {
    /* Plane-cover rule: the aperture mask and preview colours TEST at
     * plane − M, covering everything at or behind it inside the footprint.
     * This replaces the og-OT mirroring here (the far-portal +64-slot push
     * and the retired sky eps). The margin lives ONLY in this test depth:
     * nothing writes a non-true depth, and the preview's own DRAWN depth
     * never moves - pulling the whole sky forward wins ties against the
     * coincident inner rim but breaks the portal silhouette against the
     * arch everywhere else. The behind-plane constructions (spill,
     * correction band, outer ring) keep their own explicit push-back -
     * they want to LOSE honest contests. */
    double v = vz - (double)openpete_portal_plane_margin();
    if (v < 1.0) v = 1.0;
    return openpete_native_scene_viewz(openpete_worldvz_full((float)v));
}
/** @brief Returns the portal sky depth mode selected by OPENPETE_PORTAL_SKY_DEPTH.
 *
 * 0 (default, plane): per-pixel opening-plane depth with the two-region
 * spill split. 1 (a value starting with 'r', ring): the farthest outline
 * point's view Z. 2 (a value starting with 'o', ot): the sky's world-OT
 * bucket as RenderPortals computes it, B = (sum of outline SZ / npts) >> 7,
 * plus 64 when B >= 256, clamped to [1, 2047] (block_800515C4-80051674), at
 * the constant depth 128 B, combined with the plane depth by sky_min2. The
 * terrain walk buckets each face at (sum of 4 corner SZ) >> 7 plus the
 * face's (w3 & 0x38) >> 1 sort bias, with terrain SZ at world/4, so both
 * are world-scale mean view Z / 128. The ot mode does not reproduce every
 * PsyCross order: in the Alpine far-pillar case the pillar's bucket (about
 * 39) is farther than the sky's (31) and PsyCross paints the pillar on top.
 * The variable is read once and cached.
 * @return 0, 1 or 2. */
static int portal_sky_depth_mode(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_PORTAL_SKY_DEPTH");
        v = 0;                                   /* default: plane + spill split + guard */
        if (e) { if (e[0] == 'r') v = 1; else if (e[0] == 'o') v = 2; }
    }
    return v;
}
static double s_ring_vz = 0.0;   /* farthest outline vz of the current feed (0 = none) */
static double s_ot_vz   = 0.0;   /* 128 · mirrored .s OT bucket (0 = none) */
/** @brief Sets s_sky_push_vz from the feed's resolved OT depth s_ot_vz.
 *
 * s_ot_vz comes from live D_PORTAL_DEPTH, the captured hint, or the outline
 * mirror, each carrying the +64 bucket bump; sky_ot_push_for maps it to the
 * push. Called after s_ot_vz is settled and before any depth is built. */
static void set_sky_ot_push(void) { s_sky_push_vz = sky_ot_push_for(s_ot_vz); }
/** @brief Returns the scene depth of the ring sky mode: the farthest outline point pushed behind.
 *
 * The depth is s_ring_vz + s_sky_push_vz + portal_sky_eps_vz() in world
 * units, mapped through openpete_worldvz_full and
 * openpete_native_scene_viewz.
 * @return The scene depth. */
static float ring_depth(void) {
    /* +EPS (not −): the ring construction wants the sky BEHIND the outline
     * it was measured from - the same margin, opposite direction. */
    return openpete_native_scene_viewz(
               openpete_worldvz_full((float)(s_ring_vz + s_sky_push_vz + portal_sky_eps_vz())));
}
/** @brief Returns the scene depth of the ot sky mode: 128 times the mirrored OT bucket.
 *
 * Maps s_ot_vz through openpete_worldvz_full and
 * openpete_native_scene_viewz.
 * @return The scene depth. */
static float ot_depth(void) {
    /* 128 * the mirrored .s OT bucket - the literal PSX OT placement. The
     * near-band terrain that would need extra protection here is handled
     * per-face by the PORTAL_GUARD redraw (the PSX mask-bit mirror), not by
     * a depth floor (a blanket floor over-protects the non-deferred
     * through-hole floor, which the preview must cover). */
    return openpete_native_scene_viewz(openpete_worldvz_full((float)s_ot_vz));
}
/** @brief Returns the active constant sky depth mode and its depth.
 *
 * Ring mode applies when s_ring_vz is set, ot mode when s_ot_vz is set.
 * The ot depth combines with the per-pixel plane in sky_min2 as
 * min(plane depth, 128 B): the plane covers everything behind the opening,
 * the bucket caps the depth for the rect spill and far ordering, and the
 * PORTAL_GUARD redraw restores the faces the PSX mask bit protects from
 * DrawPrim.
 * @param z Receives the constant depth when the return is non-zero.
 * @return 0 none, 1 ring, 2 ot. */
static int sky_const_depth(float* z) {
    int m = portal_sky_depth_mode();
    if (m == 2 && s_ot_vz   > 0.0) { *z = ot_depth();   return 2; }
    if (m == 1 && s_ring_vz > 0.0) { *z = ring_depth(); return 1; }
    return 0;
}
/** @brief Combines the opening-plane depth with the constant sky depth by mode.
 *
 * @param plane_z The per-pixel plane depth.
 * @param cz The constant depth from sky_const_depth.
 * @param cmode The mode sky_const_depth returned.
 * @return The smaller of plane_z and cz for mode 2, cz for mode 1, else
 *         plane_z. */
static float sky_min2(float plane_z, float cz, int cmode) {
    if (cmode == 2) return plane_z < cz ? plane_z : cz;
    if (cmode == 1) return cz;
    return plane_z;
}
/** @brief Sets s_ot_vz to 128 times the sky's OT bucket mirrored from a portal's corrected outline.
 *
 * B = (mean outline view Z) >> 7, plus 64 when B >= 256, clamped to
 * [1, 2047], as RenderPortals buckets the sky. Each point's view Z is
 * floored at 0, as the GTE saturates SZ3, so a point behind the camera
 * counts as the game counts it; an outline wholly in front gives the mean
 * of its projected points. Leaves s_ot_vz unchanged when the outline is
 * unusable.
 * @param cpu The guest CPU state.
 * @param v The view to project under.
 * @param p The Portal* guest address. */
static void set_ot_sky_vz(CPUState* cpu, const ngte_view* v, uint32_t p) {
    double W[16][3];
    const int npts = portal_outline_world(cpu, p, W, 1);
    if (!npts) return;
    double sum = 0.0;
    for (int k = 0; k < npts; k++) {
        double q[3];
        portal_view_point(v, W[k], q);
        sum += (q[2] > 0.0) ? q[2] : 0.0;
    }
    long B = (long)(sum / (double)npts) >> 7;
    if (B >= 256) B += 64;
    if (B > 2047) B = 2047;
    if (B < 1) B = 1;
    s_ot_vz = 128.0 * (double)B;
}

/** @brief Returns the native depth of the portal plane at a screen point.
 *
 * Evaluates the fit_invz plane (perspective-correct), then
 * portal_cyclorama_depth pulls it nearer by the plane-cover margin
 * openpete_portal_plane_margin(). A 1/vz at or below 1e-9 reads as vz = 1e9.
 * @param a The fit's x coefficient.
 * @param b The fit's y coefficient.
 * @param c The fit's constant.
 * @param x The screen X.
 * @param y The screen Y.
 * @return The scene depth. */
static float plane_depth(double a, double b, double c, double x, double y) {
    double inv = a * x + b * y + c;
    double vz = (inv > 1e-9) ? 1.0 / inv : 1e9;
    return portal_cyclorama_depth(vz);
}

/** @brief Returns the native depth of the portal plane at a screen point, pushed behind the plane.
 *
 * The push is s_sky_push_vz + portal_sky_eps_vz(), the aperture ring's
 * construction. Serves the dome's spill outside the opening, where world
 * geometry at or near the plane must win.
 * @param a The fit's x coefficient.
 * @param b The fit's y coefficient.
 * @param c The fit's constant.
 * @param x The screen X.
 * @param y The screen Y.
 * @return The scene depth. */
static float plane_depth_back(double a, double b, double c, double x, double y) {
    double inv = a * x + b * y + c;
    double vz = (inv > 1e-9) ? 1.0 / inv : 1e9;
    return openpete_native_scene_viewz(
               openpete_worldvz_full((float)(vz + s_sky_push_vz + portal_sky_eps_vz())));
}

/* Far-path portal preview: the destination dome renders as a single flat colour
 * (a0[0]) filling the arch clip rect. Emit it as one opaque quad at the portal's
 * world-Z depth, so it sorts OVER the far terrain but UNDER the near arch stone
 * (which the terrain/moby feeders draw on top, leaving colour only in the hole).
 * Close-range portals (a0 not a RAM pointer -> the per-vertex dome path) are left
 * to the next increment. Returns 1 if a quad was emitted.
 *
 * For a CLOSE portal the original draws TWO passes: a flat colour base (the close
 * func_8004FEA0, a0=skybox+0x10) THEN the gouraud dome over it (func_8004F4BC) -
 * painter's order in the OT. With a depth buffer the flat base would land on the
 * same portal plane as the dome and z-fight (worse: the two interpolate depth
 * differently across the interior - this quad linearly per fan-tri, the dome
 * perspective-correct per pixel - so the bg colour shimmers through). So a
 * SKYBOX-LINKED flat (a0 == a portal's m_Skybox, or m_Skybox+0x10) is the dome's
 * BACKSTOP. It takes no push-back: the stencil groups' in-group painter order
 * at equal depth (the dome draws after the backstop and wins per pixel) settles
 * it. The backstop only shows through the dome's culled/near-rejected edge
 * gaps, still far nearer than the through-hole terrain. */
/* rgb_override == PORTAL_RGB_RESOLVE means "resolve the colour from a0" (the
 * canonical leaf path, where a0 is live); any other value is used directly (the
 * sub-tick reextract path, where a0 may be a stale stack/transient pointer so the
 * captured raw colour is supplied instead). portal_hint != 0 likewise supplies the
 * owning portal directly (captured) instead of re-matching a possibly-stale a0. */
#define PORTAL_RGB_RESOLVE 0xFFFFFFFFu

/* Every portal-sky tri is emitted TWICE: once as the invisible stencil
 * aperture MASK (PORTAL_MASK: depth-tested vs the world, carries the tie-break
 * z) and once as the visible colours (PORTAL_CYCLORAMA: depth-free,
 * stencil-gated in core). `s_portal_group` associates a portal element's mask
 * with its colours for core's per-group stencil isolation; it is the owning
 * portal / skybox POINTER (stable across canonical render and sub-tick
 * replay), set by each feed path immediately before it emits. File-local
 * ambient only - never crosses a feeder boundary. */
static int s_portal_group = 0;
/* Preview kind of the leaf currently feeding - the provenance tag's identity.
 * Sticky like s_portal_group: set where that is set, consumed at emit. */
static int s_portal_kind = OPENPETE_TAG_PORTAL_FLAT;
/* Set while the current feed has marked a stencil aperture from the OPENING
 * POLYGON (emit_portal_aperture): the sky tris then emit as colours only, and
 * the opening - not a depth race against the arch stone - bounds coverage.
 * Cleared per feed, so a portal with no usable outline falls back to core's
 * "a sky run without a matching mask uses its own footprint" behaviour. */
static int s_aperture_ok = 0;
static void emit_portal_aperture(CPUState* cpu, uint32_t portal, const ppt* poly, int pn, int clipped,
                                 int use_plane, double pa, double pb, double pc) OP_REQUIRES(sim);
static void outline_offset(const ppt* poly, int pn, const double* d, float* ex, float* ey);
static double portal_aperture_margin_world(void);
static double portal_margin_for(CPUState* cpu, uint32_t p);
static void margin_px_per_vertex(CPUState* cpu, const ngte_view* v, const ppt* poly, int pn,
                                 double world, double* out);
static void emit_aperture_ring(const ppt* poly, int pn, const double* dpx,
                               uint32_t rgba, int channel) OP_REQUIRES(sim);
/* Portal-guard gate for the terrain feeder (see OPENPETE_SCENE_PORTAL_GUARD):
 * true while a portal fed sky within the last couple of presents. */
static int      s_sky_fed = 0;
static uint32_t s_sky_fed_epoch = 0;

/** @brief Whether `stamp` names the canonical render in flight or the last one presented.
 *
 * A capture made during a runahead speculation carries the epoch of the
 * canonical present that shows it, where its openpete_frame leads the committed
 * tick the sub-tick re-extract runs at.
 * @param stamp a canonical-render epoch stamp
 * @return 1 when `stamp` is the current or the last committed epoch, else 0 */
static int canon_epoch_recent(uint32_t stamp) OP_REQUIRES(sim) {
    return stamp == spyro_native_canon_epoch_stamp() || spyro_native_canon_epoch_fresh(stamp);
}

int openpete_portal_sky_recent(void) OP_REQUIRES(sim) {
    if (s_pcap_pick == CUT_HELD_EMPTY) return 0;
    return s_sky_fed && canon_epoch_recent(s_sky_fed_epoch);
}
/* Plane provider for the terrain OT-conformance clamp; documented in portal.h. */
int openpete_portal_sky_planes(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max) OP_REQUIRES(sim) {
    /* OPENPETE_PORTAL_PLANES_DBG: name the reason a portal produced no plane
     * entry. Every bail here is silent otherwise, and "no colours at all" was
     * repeatedly mistaken for "the classifier rejected everything". */
    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_PORTAL_PLANES_DBG"); dbg = (e && *e && *e != '0'); }
    /* No preview paints an opening while the portal path is deselected, so
     * terrain keeps its faces there. */
    const int rp = render_path_enabled(RP_PORTAL);
    if (!rp || !s_pcap_valid || s_pcap_n == 0 || s_pcap_pick == CUT_HELD_EMPTY) {
        if (dbg) fprintf(stderr, "[pplanes f=%u] BAIL no-cap rp=%d valid=%d n=%d\n", openpete_frame, rp, s_pcap_valid, s_pcap_n);
        return 0;
    }
    if (s_pcap_pick < 0 && !canon_epoch_recent(s_pcap_epoch)) {
        if (dbg) fprintf(stderr, "[pplanes f=%u] BAIL stale capframe=%u\n", openpete_frame, s_pcap_frame);
        return 0;
    }
    /* Everything this provider publishes describes the AUTHORED opening
     * (nudge=0). Its consumers ask about the MESH - is this face part of the
     * arch's inner rim, does the preview cover this terrain - and the answer
     * must not move when a portal is repositioned for art reasons. Only the
     * RENDER may use the corrected outline. OPENPETE_PORTAL_RIM_NUDGED=1
     * follows the corrected one instead, for A/B. */
    static int s_rimnud = -1;
    if (s_rimnud < 0) { const char* e2 = getenv("OPENPETE_PORTAL_RIM_NUDGED");
                        s_rimnud = (e2 && *e2 && *e2 != '0'); }
    int n = 0;
    for (int i = 0; i < s_pcap_n && n < max; i++) {
        uint32_t p = s_pcap[i].portal;
        if (!p) { if (dbg) fprintf(stderr, "[pplanes f=%u] cap%d no-portal\n", openpete_frame, i); continue; }
        int dup = 0;
        for (int j = 0; j < n; j++) if (out[j].portal == p) { dup = 1; break; }
        if (dup) continue;
        openpete_portal_skyplane* o = &out[n];
        o->rim_fragment = 0;               /* an accepted rim face is dropped whole */
        if (!portal_plane_invz(cpu, v, p, &o->a, &o->b, &o->c)) {
            if (dbg) fprintf(stderr, "[pplanes f=%u] cap%d p=%08X plane-FAIL\n", openpete_frame, i, p);
            continue;
        }
        o->back_vz = portal_outline_maxvz(cpu, v, p, s_rimnud);
        if (o->back_vz <= 1.0) {              /* whole outline behind the camera */
            if (dbg) fprintf(stderr, "[pplanes f=%u] cap%d p=%08X back_vz=%.1f\n", openpete_frame, i, p, o->back_vz);
            continue;
        }
        /* The outline in VIEW space, which - unlike the projection below - is
         * defined for every camera. Any consumer that works in the opening
         * plane can use this at ranges where the projection has failed. */
        o->vn = 0;
        {
            double Wv[16][3];
            const int npts = portal_outline_world(cpu, p, Wv, s_rimnud);
            if (npts) {
                const double ccx = v->cam_ram[0], ccy = v->cam_ram[1], ccz = v->cam_ram[2];
                for (int k = 0; k < npts; k++)
                    ngte_mac(v->cam_M, -(Wv[k][1] - ccy), -(Wv[k][2] - ccz), (Wv[k][0] - ccx),
                             &o->vx[k], &o->vy[k], &o->vz[k]);
                o->vn = npts;
            }
        }
        ppt poly[16];
        /* The conformance sink swallows terrain that the preview covers. Fed
         * the CORRECTED outline it swallowed arch stone: a correction reaches
         * outward to meet the arch's inner edge, so faces of the jamb itself
         * fall inside it, get sunk behind the preview, and the sky eats a
         * wedge out of the stone (artisans->stone_hill, whose opening is the
         * most-corrected of the ten). The sink must ask where the ORIGINAL
         * opening was, exactly as the rim classifier does. */
        int pn = s_rimnud ? portal_outline_screen(cpu, v, p, poly, 16)
                          : portal_outline_screen_raw(cpu, v, p, poly, 16);
        if (dbg && pn < 3) fprintf(stderr, "[pplanes f=%u] cap%d p=%08X pn=%d vn=%d\n", openpete_frame, i, p, pn, o->vn);
        /* The conformance sink needs the PROJECTED polygon (coverage
         * confinement) - with a point behind the camera there is none, so no
         * sinking for that portal (point-blank/entering: the near-cover path
         * rules). The entry is still published when the view-space outline
         * exists, because consumers that work in the plane are perfectly able
         * to run at that range and were losing the portal exactly when the
         * camera got close to it. A pn=0 entry is inert to the sink, which
         * already requires pn >= 3. */
        if (pn < 3) {
            if (o->vn < 3) continue;
            o->pn = 0; o->ot_vz = 0.0; o->push_vz = 0.0;
            o->x0 = o->y0 = -1e9f; o->x1 = o->y1 = 1e9f;
            o->portal = p;
            n++;
            continue;
        }
        double sum = 0.0;                     /* mirror the .s sky bucket B */
        for (int k = 0; k < pn; k++) sum += poly[k].vz;
        long B = (long)(sum / (double)pn) >> 7;
        if (B >= 256) B += 64;
        if (B > 2047) B = 2047;
        if (B < 1) B = 1;
        o->ot_vz = 128.0 * (double)B;
        o->push_vz = sky_ot_push_for(o->ot_vz);
        float x0 = poly[0].x, x1 = poly[0].x, y0 = poly[0].y, y1 = poly[0].y;
        for (int k = 1; k < pn; k++) {
            if (poly[k].x < x0) x0 = poly[k].x;
            if (poly[k].x > x1) x1 = poly[k].x;
            if (poly[k].y < y0) y0 = poly[k].y;
            if (poly[k].y > y1) y1 = poly[k].y;
        }
        o->x0 = x0; o->x1 = x1; o->y0 = y0; o->y1 = y1;
        o->pn = pn;
        for (int k = 0; k < pn; k++) { o->px[k] = poly[k].x; o->py[k] = poly[k].y; }
        o->portal = p;
        n++;
    }
    return n;
}
/** @brief Reports whether --render-paths enables RP_PORTAL for the native scene emission.
 *
 * Applies the flag to the native feed as well as the OT walk, so an
 * isolation run without portals draws no destination-sky previews over the
 * arch geometry. Captures, the plane provider and canon-drawn marks run
 * regardless; only the scene emission is suppressed.
 * @return Non-zero when portal triangles are emitted. */
static int portal_scene_emit_enabled(void) { return render_path_enabled(RP_PORTAL); }

/* Portal triangles emitted since startup, colour and mask; a replay
 * differences them around each capture's feed for the sub-tick record. */
static unsigned s_tri_col, s_tri_mask;

/** @brief Adds one portal triangle to the scene in the current portal group.
 *
 * Every portal triangle lies on, or is constructed against, the opening
 * plane at its own per-vertex depth, so each vertex's perspective depth
 * (pvz) is set to its stored depth.
 * @param t The triangle.
 * @param category The scene category (mask or cyclorama). */
static void portal_add_tri(const openpete_scene_tri* t, int category) OP_REQUIRES(sim) {
    if (category == OPENPETE_SCENE_PORTAL_MASK) s_tri_mask++; else s_tri_col++;
    openpete_scene_tri w = *t;
    for (int k = 0; k < 3; k++) w.v[k].pvz = w.v[k].z;
    openpete_native_scene_add_tri_grouped(&w, category, 0, s_portal_group);
}

/** @brief Adds one sky triangle to the portal cyclorama, and to the mask when no aperture is marked.
 *
 * Does nothing unless RP_PORTAL is enabled. The triangle carries the
 * OPENPETE_TAG_PORTAL tag of s_portal_kind. With s_aperture_ok clear, the
 * triangle's footprint is also its mask and is added to
 * OPENPETE_SCENE_PORTAL_MASK first.
 * @param t The triangle, with depth in each vertex's z. */
static void portal_add_sky_tri(const openpete_scene_tri* t) OP_REQUIRES(sim) {
    if (!portal_scene_emit_enabled()) return;
    openpete_native_scene_set_tag(OPENPETE_TAG_PORTAL(s_portal_kind, 0));
    if (!s_aperture_ok)   /* no opening polygon this feed: footprint IS the mask */
        portal_add_tri(t, OPENPETE_SCENE_PORTAL_MASK);
    portal_add_tri(t, OPENPETE_SCENE_PORTAL_CYCLORAMA);
    openpete_native_scene_set_tag(0);
}

static int band_dbg(void);

/** @brief Reports whether OPENPETE_PORTAL_REPR suppresses a preview representation.
 *
 * OPENPETE_PORTAL_REPR=dome (a value starting with 'd') emits only the
 * dome, any other non-empty value only the flat fill. The flat and the
 * dome cross-dissolve, so their footprints must agree at every angle and
 * distance; a band one paints and the other does not shows as a rim of
 * the outgoing colour mid-dissolve. Rendering each alone makes the
 * footprint comparison a pixel count. The variable is read once and cached.
 * @param is_dome Non-zero for the dome, 0 for the flat fill.
 * @return 1 when this representation is skipped, else 0. */
static int portal_repr_skip(int is_dome) {
    static int m = -2;
    if (m == -2) {
        const char* e = getenv("OPENPETE_PORTAL_REPR");
        m = (!e || !*e) ? -1 : (e[0] == 'd' ? 1 : 0);
    }
    return (m >= 0) && ((m == 1) != (is_dome != 0));
}

/** @brief Feed one portal leaf's flat sky fill to the native scene.
 *
 * Resolves the fill colour and its portal, projects the opening outline, marks
 * the stencil aperture and emits the outline fan at per-vertex opening depth,
 * stamping the portal-guard recency with the canonical-render epoch. The
 * outline is clipped at the near plane (portal_outline_clipped); when no part
 * of it lies in front of the camera it fills the clip rect instead, mapped
 * onto an extended view (portal_fallback_rect), at the opening plane's depth,
 * or nearest when the camera has crossed the plane.
 * Skipped under OPENPETE_PORTAL_REPR=dome.
 * @param cpu          guest CPU state
 * @param a0           the leaf's a0: a pointer to the fill colour, or the skybox
 *                     record when `a0_is_skybox`
 * @param a0_is_skybox 1 when a0 is the skybox record (colour at +0x18)
 * @param portal_hint  the Portal record when the caller knows it, else 0 to
 *                     resolve it from a0 and the clip rect
 * @param rgb_override the fill colour, or PORTAL_RGB_RESOLVE to read it from a0
 *                     and take the sky bucket from the live D_PORTAL_DEPTH
 * @param ot_b_hint    the captured sky OT bucket when rgb_override is given,
 *                     else 0
 * @param fog_rgb      fog colour the fill is blended toward
 * @param fog_t        blend fraction toward fog_rgb
 * @return 1 when the quad was fed, 0 when skipped or the clip rect is empty */
static int feed_portal_quad(CPUState* cpu, uint32_t a0, int a0_is_skybox,
                            uint32_t portal_hint,
                            uint32_t rgb_override, uint32_t ot_b_hint,
                            uint32_t fog_rgb, double fog_t) OP_REQUIRES(sim) {
    if (portal_repr_skip(0)) return 0;
    if (rgb_override == PORTAL_RGB_RESOLVE &&
        (a0 < 0x80010000u || a0 >= 0x80200000u)) return 0;   /* canonical: a0 must be a RAM pointer */
    /* The flat-fill colour. The FLAT leaf (func_8004FEA0) passes a pointer
     * whose [0] IS the colour (stack fog colour, &D_800757D4 blend, or the
     * close-pass skybox+0x10 bg colour); the dome-family leaves (func_8004F4BC
     * close, func_80050240 blended-medium) pass the skybox struct itself, whose
     * m_BackgroundColor lives at +0x18 - a0_is_skybox comes from the leaf
     * hook's kind. On the reextract a0 / D_800757D4 are stale, so the captured
     * raw colour is passed in directly. */
    uint32_t rgb = (rgb_override != PORTAL_RGB_RESOLVE)
                       ? (rgb_override & 0xFFFFFF)
                       : portal_flat_raw_rgb(cpu, a0, a0_is_skybox);
    rgb = blend_to_fog(rgb, fog_rgb, fog_t);     /* blended-medium: fade the base toward the fog colour */
    uint32_t rgba = rgb | 0xFF000000u;

    int y0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x0) >> 16;
    int y1 = (int32_t)cpu->read_word(D_CLIPRECT + 0x4) >> 16;
    int x0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x8) >> 16;
    int x1 = (int32_t)cpu->read_word(D_CLIPRECT + 0xC) >> 16;
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }      /* rect is min/max either order */
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    if (x1 <= x0 || y1 <= y0) return 0;                /* empty/degenerate */

    /* The portal opening is a tilted planar window. Fill its actual outline
     * polygon (projected m_Points) with per-vertex depth = each point's view-Z,
     * so at a sharp angle the near (bottom) edge keeps its near depth and the
     * floor occludes it exactly at the frame, instead of a single flat depth
     * that makes the floor eat the near part. */
    uint32_t p = portal_hint ? portal_hint
                             : owning_portal_ptr(cpu, &s_view, a0, (x0 + x1) * 0.5, (y0 + y1) * 0.5);
    /* A skybox-linked flat (the dome's backstop) takes no push-back: within a
     * stencil group the colours draw in submission order (backstop first, dome
     * last) with LEQUAL, so at EQUAL plane depth the dome wins per pixel.
     * Everything sits on the opening plane. */
    /* Group id for mask<->colour pairing: the destination SKYBOX pointer, so a
     * portal's flat base, backstop AND dome (fed by separate leaves with
     * different a0s) all land in ONE stencil group - within a group the
     * colours composite by submission order (backstops fed before the dome ->
     * dome paints last). Keying the quad by portal and the dome by skybox
     * would split one portal into two groups, and the far->near inter-group
     * sort could then put the dome FIRST (its farthest face can sort deeper
     * than the backstop), letting the flat backstop paint over the dome. */
    s_portal_group = (int)(p ? cpu->read_word(p + 0x00) : a0);
    ppt poly[PORTAL_POLY_MAX];
    int clipped = 0;
    int pn = p ? portal_outline_clipped(cpu, &s_view, p, 1, poly, PORTAL_POLY_MAX, &clipped) : 0;
    /* The sky's OT bucket: canonical leaves read the LIVE D_PORTAL_DEPTH the
     * .s just computed for THIS portal (correct even when the outline is
     * unavailable - a point behind the camera at close/steep range); the
     * sub-tick replay uses the captured bucket; the outline mirror (validated
     * == live) is the last resort. A stale s_ot_vz left by another portal
     * would place the sky far too deep and let the through-hole floor beat it. */
    if (rgb_override == PORTAL_RGB_RESOLVE)
        s_ot_vz = 128.0 * (double)(int32_t)cpu->read_word(D_PORTAL_DEPTH);
    else if (ot_b_hint)
        s_ot_vz = 128.0 * (double)ot_b_hint;
    else
        s_ot_vz = 0.0;
    if (pn >= 3) {
        portal_clip_note(PORTAL_CLIP_FRONT + (clipped != 0));
        if (s_ot_vz <= 0.0) set_ot_sky_vz(cpu, &s_view, p);
        set_sky_ot_push();
        /* Mirror check (canonical only; a0 live means the .s just computed
         * D_PORTAL_DEPTH for THIS portal): our bucket must equal the .s's. */
        if (portal_dump_enabled() && rgb_override == PORTAL_RGB_RESOLVE) {
            static int chk = 0;
            long liveB = (long)(int32_t)cpu->read_word(D_PORTAL_DEPTH);
            long mirB  = (long)(s_ot_vz / 128.0);
            if (chk < 8 && liveB != mirB) {
                chk++;
                fprintf(stderr, "[portal-otchk f=%u] MISMATCH mirrored=%ld live=%ld\n",
                        openpete_frame, mirB, liveB);
            } else if (chk < 8 && liveB == mirB) {
                chk++;
                fprintf(stderr, "[portal-otchk f=%u] ok B=%ld (vz %.0f)\n",
                        openpete_frame, mirB, s_ot_vz);
            }
        }
        /* Coverage = the OUTLINE FAN. The .s flat leaf fans the OPENING
         * POLYGON, not the clip rect: below the outline's bottom edge the sky
         * never rasters and the .s shows the floor/sill there (a rect fill
         * would over-cover that band). A wedge-class artifact at the base is
         * a depth-test effect, handled by the terrain OT-conformance sink. */
        s_ring_vz = 0.0;
        for (int i = 0; i < pn; i++) if (poly[i].vz > s_ring_vz) s_ring_vz = poly[i].vz;
        s_sky_fed = 1; s_sky_fed_epoch = spyro_native_canon_epoch_stamp();
        float cz = 0.0f;
        const int cmode = sky_const_depth(&cz);
        emit_portal_aperture(cpu, p, poly, pn, clipped, 0, 0.0, 0.0, 0.0);
        /* Interior fill = the EXACT outline fan (per-vertex view-Z, the .s
         * flat leaf's own construction); erosion (debug) offsets it to
         * match the eroded mask. The dilation band's colours come from the
         * behind-depth ring below, matching the mask ring's extent. */
        const double fmargin = portal_margin_for(cpu, p);
        double fpx[PORTAL_POLY_MAX], ferode[PORTAL_POLY_MAX];
        margin_px_per_vertex(cpu, &s_view, poly, pn, fmargin, fpx);
        const double fsign = (s_aperture_ok && fmargin < 0.0) ? -1.0 : 0.0;   /* inward */
        for (int i = 0; i < pn; i++) ferode[i] = fsign * fpx[i];
        float fx[PORTAL_POLY_MAX], fy[PORTAL_POLY_MAX];
        outline_offset(poly, pn, ferode, fx, fy);
        double qa = 0.0, qb = 0.0, qc = 0.0;
        const int fan_plane = s_aperture_ok && fit_invz(poly, pn, &qa, &qb, &qc);
        for (int i = 1; i + 1 < pn; i++) {             /* fan-triangulate the outline */
            openpete_scene_tri t = {0};
            memset(&t, 0, sizeof t);
            t.textured = 0; t.blend = 0;
            const int idx3[3] = { 0, i, i + 1 };
            for (int k = 0; k < 3; k++) {
                const int j = idx3[k];
                t.v[k].sx = fx[j]; t.v[k].sy = fy[j];
                t.v[k].z = sky_min2(fan_plane ? plane_depth(qa, qb, qc, fx[j], fy[j])
                                              : portal_cyclorama_depth(poly[j].vz), cz, cmode);
                t.v[k].rgba = band_dbg() ? 0xFFFF0000u : rgba;   /* BLUE = flat interior fan */
            }
            portal_add_sky_tri(&t);
        }
        if (s_aperture_ok && fmargin > 0.0)
            emit_aperture_ring(poly, pn, fpx, band_dbg() ? 0xFF0000FFu : rgba,  /* RED = flat ring */
                               OPENPETE_SCENE_PORTAL_CYCLORAMA);
        return 1;
    }

    /* Fallback (the whole outline behind the camera / no match): the
     * axis-aligned clip rect, mapped onto an extended view
     * (portal_fallback_rect). vz<=1 means the plane centre is AT/BEHIND the camera - Spyro
     * is entering and the camera has crossed the portal: the preview must
     * cover the level being left (the PSX sky bucket was nearest-OT at that
     * point), so go NEAR, not far (a far depth would let current-level
     * mobys repaint over the preview mid-entry). The sky
     * repaints after the ACTOR group; the PLAYER group draws later, so Spyro
     * himself is never covered.
     *
     * Depth: the ANALYTIC opening plane when available (computed from the
     * outline's world points, so points behind the camera don't invalidate
     * it) - a close, acutely-angled portal keeps per-pixel plane depth and
     * the floor still meets the opening's near side instead of eating it (one
     * constant farthest-outline depth would let the floor eat it).
     * Constant farthest-outline depth remains the last resort. */
    double vz = portal_viewz(cpu, &s_view, a0, (x0 + x1) * 0.5, (y0 + y1) * 0.5);
    int crossed = (vz <= 1.0);
    if (crossed) vz = 2.0;     /* crossed -> nearest */
    portal_fallback_rect(&s_view, &x0, &y0, &x1, &y1);
    set_sky_ot_push();
    if (crossed) s_sky_push_vz = 0.0;   /* mid-entry cover must win everything */
    float depth = portal_cyclorama_depth(vz);
    float cz3 = 0.0f;
    int cm3 = sky_const_depth(&cz3);
    /* Constant-depth modes apply to the fallback rect too (the pn<3 case IS
     * the common close/steep one) - except mid-entry (crossed), where the
     * near cover must win everything. */
    if (!crossed && cm3) depth = sky_min2(depth, cz3, cm3);
    double fa = 0, fb = 0, fc = 0;
    int fplane = (!crossed && p) && portal_plane_invz(cpu, &s_view, p, &fa, &fb, &fc);
    float cxx[4] = { (float)x0, (float)x1, (float)x1, (float)x0 };
    float cyy[4] = { (float)y0, (float)y0, (float)y1, (float)y1 };
    int idx[2][3] = { {0,1,2}, {0,2,3} };
    for (int ti = 0; ti < 2; ti++) {
        openpete_scene_tri t = {0};
        memset(&t, 0, sizeof t);
        t.textured = 0; t.blend = 0;
        for (int k = 0; k < 3; k++) {
            float z = depth;
            if (fplane) {
                double inv = fa * cxx[idx[ti][k]] + fb * cyy[idx[ti][k]] + fc;
                if (inv > 1e-9) {           /* plane behind camera at this corner -> constant */
                    z = portal_cyclorama_depth(1.0 / inv);
                    if (cm3) z = sky_min2(z, cz3, cm3);
                }
            }
            t.v[k].sx = cxx[idx[ti][k]]; t.v[k].sy = cyy[idx[ti][k]];
            t.v[k].z = z; t.v[k].u = 0.0f; t.v[k].v = 0.0f; t.v[k].rgba = rgba;
        }
        portal_add_sky_tri(&t);
    }
    return 1;
}

/* ---- close-range portal: the full per-vertex gradient DOME --------------- */
/* When the camera is near a portal, func_8004F4BC re-projects the destination
 * skybox dome (gouraud, with clouds/horizon) instead of a flat colour. It's the
 * same cyclorama dome FORMAT as the main sky (cull pt + centre header + packed
 * 11/11/10 verts + F3/G3 faces), so this mirrors cyclorama_native's cyc_extract:
 * matrix from the GTE the leaf left (post-call, TR=0), sectors from the skybox
 * (count@+0x00, sector-array ptr@+0x04), gouraud faces Sutherland-Hodgman-clipped
 * to the arch rect at the portal depth. */
/** @brief Clips a convex gouraud polygon against one axis-aligned half-plane.
 *
 * Sutherland-Hodgman, interpolating position and colour at each crossing.
 * @param in The polygon.
 * @param n Its vertex count.
 * @param out Receives the clipped polygon; holds up to n + 1 vertices.
 * @param axis 0 = x, 1 = y.
 * @param val The plane's coordinate.
 * @param keepGE 1 keeps the >= val side, 0 the <= val side.
 * @return The clipped vertex count. */
static int clip_half(const pgv* in, int n, pgv* out, int axis, float val, int keepGE) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const pgv* a = &in[i];
        const pgv* b = &in[(i + 1) % n];
        float av = axis ? a->y : a->x, bv = axis ? b->y : b->x;
        int ain = keepGE ? (av >= val) : (av <= val);
        int bin = keepGE ? (bv >= val) : (bv <= val);
        if (ain) out[m++] = *a;
        if (ain != bin) {
            float t = (bv != av) ? (val - av) / (bv - av) : 0.0f;
            pgv c;
            c.x = a->x + t * (b->x - a->x);  c.y = a->y + t * (b->y - a->y);
            c.r = a->r + t * (b->r - a->r);  c.g = a->g + t * (b->g - a->g);  c.b = a->b + t * (b->b - a->b);
            out[m++] = c;
        }
    }
    return m;
}

/* ---- two-region sky depth (the PSX OT placement) --------------------------
 * The .s sorts the portal sky BEHIND the whole stone ring (its OT key is
 * derived from the ring - see portal_outline_maxvz), so the rect spill over
 * the arch is repainted by ANY nearer geometry (arch stone, threshold floor)
 * while the through-hole backdrop (farther than the ring) is covered. One
 * depth for the whole sky cannot serve both: plane depth everywhere lets the
 * sky paint over terrain behind the opening plane at steep angles (the arch's
 * far pillar), and constant behind-ring depth everywhere lets the receding
 * floor behind the plane but nearer than the ring's far point poke through
 * the preview inside the opening.
 * The dome is not split by the outline: the stencil aperture (below) bounds
 * its coverage and emit_split_by_outline gives every face plane depth.
 * register_skyout still records the projected outline in s_skyout, which no
 * code reads. */
static pgv   s_skyout[PORTAL_POLY_MAX]; /* outline polygon (screen space), CCW-oriented */
static int   s_skyout_n = 0;      /* 0 = unavailable -> single-depth fallback */

/** @brief Register the projected outline for the two-region split, oriented CCW by the centroid side.
 *
 * Serves both the dome and the flat-fill paths. Also sets s_ring_vz to the
 * outline's largest view-Z and stamps the portal-sky recency
 * openpete_portal_sky_recent reads with the canonical-render epoch. */
static void register_skyout(const ppt* poly, int pn) OP_REQUIRES(sim) {
    double maxvz = 0.0, mx = 0.0, my = 0.0;
    for (int i = 0; i < pn; i++) {
        if (poly[i].vz > maxvz) maxvz = poly[i].vz;
        mx += poly[i].x; my += poly[i].y;
    }
    mx /= pn; my /= pn;
    /* orient so the polygon interior (its centroid) is LEFT of edge 0, the
     * side where (p1 - p0) x (p - p0) >= 0 */
    double sgn = ((double)poly[1].x - poly[0].x) * (my - poly[0].y)
               - ((double)poly[1].y - poly[0].y) * (mx - poly[0].x);
    for (int i = 0; i < pn; i++) {
        const ppt* q = (sgn >= 0.0) ? &poly[i] : &poly[pn - 1 - i];
        s_skyout[i].x = q->x; s_skyout[i].y = q->y;
        s_skyout[i].r = s_skyout[i].g = s_skyout[i].b = 0.0f;
    }
    s_skyout_n = pn;
    s_ring_vz = maxvz;
    s_sky_fed = 1; s_sky_fed_epoch = spyro_native_canon_epoch_stamp();
}

/* ---- stencil aperture from the opening polygon ----------------------------
 * The dome leaves are confined only by the GPU scissor rect (func_80050BD0
 * zeroes D_80077EA0[0], so func_8005060C's edge clip is skipped), and the
 * PSX relies on the arch stone repainting the rect spill by OT painter
 * order. A depth buffer cannot reproduce that with one spill depth: pinned
 * to the farthest outline point plus the margin, a portal seen at an angle
 * has jamb just outside the opening that recedes past that depth, so the
 * sky wins there and shows through the arch shoulder on the far side.
 *
 * The stencil aperture is therefore marked from the opening polygon, not
 * from the sky footprint, so the silhouette is bounded geometrically and no
 * depth race decides it. The .s flat leaf does the same: it fans the
 * opening polygon, not the clip rect.
 *
 * The margin is an edge-normal offset, not a scale about the centroid: a
 * scale moves the edges in proportion to arch size and would lift the sky
 * off the sill, which the terrain OT-conformance sink depends on meeting.
 * The mask stays depth-tested, so anything in front of the opening (Spyro
 * mid-entry, a moby, the threshold lip) still punches it out. */
/** @brief Reports whether the stencil aperture from the opening polygon is enabled.
 *
 * On unless OPENPETE_PORTAL_APERTURE starts with '0'. Read once and cached.
 * @return 1 when enabled, else 0. */
static int portal_aperture_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL_APERTURE"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}
/** @brief Returns the aperture margin in world units.
 *
 * The margin exists because the authored outline can sit slightly inside
 * the arch's visible opening, leaving a seam of through-hole world down the
 * jamb. It is in world units, not screen pixels: a constant screen margin
 * is a fixed fraction of the screen, so as an arch shrinks with distance
 * the margin grows relative to it and the preview spreads past the stone.
 * margin_px_per_vertex converts it per vertex at emit time (d * H / vz), so
 * it keeps a constant relationship to the stone at every distance.
 *
 * A positive margin emits an outward ring around the opening
 * (emit_aperture_ring), so the sky reaches at most margin past the opening
 * and the mask stays depth-tested. A negative margin erodes the stencil
 * interior and the flat fan inward by |margin| (outline_offset with a
 * positive distance) and emits no ring. Default 40, overridden by
 * OPENPETE_PORTAL_APERTURE_MARGIN, read once.
 * @return The margin in world units. */
static double portal_aperture_margin_world(void) {
    static int init = 0;
    static double v = 40.0;
    if (!init) {
        const char* e = getenv("OPENPETE_PORTAL_APERTURE_MARGIN");
        if (e && *e) v = atof(e);
        init = 1;
    }
    return v;
}
/** @brief Converts a world-unit margin to a screen offset at each outline vertex's depth.
 *
 * out[i] = world * H_eff / max(vz, 1): the screen length of a world
 * displacement perpendicular to the view at that vertex.
 * @param cpu Unused.
 * @param v The running view (H_eff).
 * @param poly The projected outline.
 * @param pn The point count.
 * @param world The margin in world units.
 * @param out Receives pn screen offsets. */
static void margin_px_per_vertex(CPUState* cpu, const ngte_view* v, const ppt* poly, int pn,
                                 double world, double* out) {
    (void)cpu;
    const double H = v->H_eff;
    for (int i = 0; i < pn; i++) {
        const double vz = (poly[i].vz > 1.0) ? poly[i].vz : 1.0;
        out[i] = world * H / vz;
    }
}
/** @brief Reports whether a portal's arch geometry animates.
 *
 * The dragon-head arches, destination levelId 14 (artisans -> toasty) and
 * 62/63/64 (gnorc_gnexus -> twilight_harbor / gnasty_gnorc / gnastys_loot),
 * stretch as they open while the opening polygon does not track the mesh,
 * so a margin wide enough to close authoring seams elsewhere spills the
 * preview past the mouth. portal_margin_for gives them no dilation: the
 * aperture is exactly the authored polygon. This is not a boss-portal rule:
 * doctor_shemp (24), blowhard (34), metalhead (44) and jacques (54) are
 * static arches and take the normal margin.
 * @param cpu The guest CPU state.
 * @param p The Portal* guest address; its destination levelId is at +0x1C.
 * @return 1 for an animated arch, 0 otherwise or when p is 0. */
static int portal_arch_animates(CPUState* cpu, uint32_t p) {
    if (!p) return 0;
    const int lid = (int32_t)cpu->read_word(p + 0x1C);
    return lid == 14 || lid == 62 || lid == 63 || lid == 64;
}
#define PORTAL_ANIM_ARCH_MARGIN_WORLD 0.0
/** @brief Returns the aperture dilation margin for a portal, in world units.
 *
 * The global margin from portal_aperture_margin_world, except that a
 * positive margin is replaced by PORTAL_ANIM_ARCH_MARGIN_WORLD (0) for an
 * animated arch. A negative margin applies to every portal.
 * @param cpu The guest CPU state.
 * @param p The Portal* guest address; 0 takes the global margin.
 * @return The margin in world units. */
static double portal_margin_for(CPUState* cpu, uint32_t p) {
    const double m = portal_aperture_margin_world();
    if (m > PORTAL_ANIM_ARCH_MARGIN_WORLD && portal_arch_animates(cpu, p))
        return PORTAL_ANIM_ARCH_MARGIN_WORLD;
    return m;
}

/** @brief Offsets each outline vertex along its inward angle bisector.
 *
 * The bisector averages the two adjacent edges' normals, each oriented
 * toward the centroid, so winding order does not matter. A negative
 * distance pushes outward, and then a vertex never moves down: the
 * opening's base sits on the sill, and a fill pushed below it lands inside
 * nearer ground and breaks the junction into a staircase. The stencil
 * aperture and the flat background fan share this offset so both reach the
 * same extent: the fan is the only cover of the opening below the dome's
 * horizon.
 * @param poly The projected outline.
 * @param pn The point count.
 * @param d The per-vertex offset in pixels; negative is outward.
 * @param ex Receives the offset X per vertex.
 * @param ey Receives the offset Y per vertex. */
static void outline_offset(const ppt* poly, int pn, const double* d, float* ex, float* ey) {
    double cx = 0.0, cy = 0.0;
    for (int i = 0; i < pn; i++) { cx += poly[i].x; cy += poly[i].y; }
    cx /= pn; cy /= pn;
    for (int i = 0; i < pn; i++) {
        const ppt* pv = &poly[(i + pn - 1) % pn];
        const ppt* cv = &poly[i];
        const ppt* nv = &poly[(i + 1) % pn];
        /* average the two adjacent edges' inward normals (oriented by the
         * centroid side, so winding order doesn't matter) and step along it */
        double nx = 0.0, ny = 0.0;
        const double ax = cv->x - pv->x, ay = cv->y - pv->y;
        const double bx = nv->x - cv->x, by = nv->y - cv->y;
        double n1x = -ay, n1y = ax, n2x = -by, n2y = bx;
        const double l1 = sqrt(n1x*n1x + n1y*n1y), l2 = sqrt(n2x*n2x + n2y*n2y);
        if (l1 > 1e-6) {
            n1x /= l1; n1y /= l1;
            if ((cx - cv->x) * n1x + (cy - cv->y) * n1y < 0.0) { n1x = -n1x; n1y = -n1y; }
            nx += n1x; ny += n1y;
        }
        if (l2 > 1e-6) {
            n2x /= l2; n2y /= l2;
            if ((cx - cv->x) * n2x + (cy - cv->y) * n2y < 0.0) { n2x = -n2x; n2y = -n2y; }
            nx += n2x; ny += n2y;
        }
        const double l = sqrt(nx*nx + ny*ny);
        if (l > 1e-6) { nx /= l; ny /= l; } else { nx = 0.0; ny = 0.0; }
        double ox = nx * d[i], oy = ny * d[i];
        /* Never push the outline DOWN when dilating. The opening's base sits on
         * the sill, and the sky/ground junction there must stay a straight line
         * on the authored edge: pushed below the sill the fill lands inside
         * ground that is nearer, loses the depth test along terrain triangle
         * edges, and the junction breaks into a staircase. The sides and top
         * still spill to meet the stone, which is all the margin exists for.
         * (Erosion keeps the plain offset - it pulls in, never past the sill.) */
        if (d[i] < 0.0 && oy > 0.0) oy = 0.0;
        ex[i] = (float)(cv->x + ox);
        ey[i] = (float)(cv->y + oy);
    }
}

/** @brief Emits the dilation band between the outline and its outward offset as a ring of quads behind the plane.
 *
 * Each ring vertex sits at its own outline point's view-Z plus
 * s_sky_push_vz + portal_sky_eps_vz(), so the ring is a skirt following the
 * opening plane, just behind it. Some arches author the opening polygon a
 * little inside the visible stone edge, so the sky clipped strictly to the
 * outline leaves a sliver of background along the jamb. The band must not
 * carry the interior's depth: the interior tests at the plane pulled
 * nearer by the plane-cover margin, where it beats any stone at or near the
 * plane and paints over the jamb. Behind the plane the contest resolves
 * per pixel: stone that rasters in the band is nearer and wins, and a true
 * authoring gap shows background terrain far behind the plane, which the
 * ring beats and fills.
 *
 * The ring is bounded to the band. A full spill mask would delegate the
 * whole silhouette to the terrain bucket-mirror verdict, which misjudges
 * real faces; the ring keeps the contest local to the band, where the
 * mirror is never consulted. Emits nothing when RP_PORTAL is filtered out.
 * @param poly The projected outline.
 * @param pn The point count.
 * @param dpx The per-vertex outward offset in pixels.
 * @param rgba The ring colour.
 * @param channel The scene category (mask or cyclorama). */
static void emit_aperture_ring(const ppt* poly, int pn, const double* dpx,
                               uint32_t rgba, int channel) OP_REQUIRES(sim) {
    if (!portal_scene_emit_enabled()) return;
    openpete_native_scene_set_tag(OPENPETE_TAG_PORTAL(s_portal_kind, 0));
    float rx[PORTAL_POLY_MAX], ry[PORTAL_POLY_MAX];
    double outward[PORTAL_POLY_MAX] = { 0 };
    for (int i = 0; i < pn; i++) outward[i] = -dpx[i];
    outline_offset(poly, pn, outward, rx, ry);
    for (int i = 0; i < pn; i++) {
        const int j = (i + 1) % pn;
        /* PER-VERTEX depth, following the opening plane.
         *
         * A ring at one constant depth (the FARTHEST outline point plus the
         * bias) is wrong on an arch seen at an angle: the opening is tilted, so
         * the ring sits far BEHIND the plane along the near jamb, and anything
         * in that void beats it, leaving a 1-2px seam of through-hole world
         * along the near side.
         *
         * Each ring vertex carries its own outline point's view-Z, so the
         * ring is a skirt on the plane rather than a flat card behind it, and
         * the stone-versus-gap contest resolves the same way at every point
         * around the opening. */
        const double vzi = poly[i].vz + s_sky_push_vz + portal_sky_eps_vz();
        const double vzj = poly[j].vz + s_sky_push_vz + portal_sky_eps_vz();
        float zi = openpete_native_scene_viewz(openpete_worldvz_full((float)vzi));
        float zj = openpete_native_scene_viewz(openpete_worldvz_full((float)vzj));
        const float qx[4] = { (float)poly[i].x, (float)poly[j].x, rx[j], rx[i] };
        const float qy[4] = { (float)poly[i].y, (float)poly[j].y, ry[j], ry[i] };
        const float qz[4] = { zi, zj, zj, zi };
        for (int half = 0; half < 2; half++) {   /* zero-area halves (a no-offset
                                                  * bottom edge) raster nothing */
            const int ks[3] = { 0, half ? 2 : 1, half ? 3 : 2 };
            openpete_scene_tri t;
            memset(&t, 0, sizeof t);
            t.textured = 0; t.blend = 0;
            for (int k = 0; k < 3; k++) {
                t.v[k].sx = qx[ks[k]]; t.v[k].sy = qy[ks[k]];
                t.v[k].z = qz[ks[k]]; t.v[k].rgba = rgba;
            }
            portal_add_tri(&t, channel);
        }
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Reports whether a portal's correction moves its opening, so the aperture splits at the authored outline.
 *
 * With both outlines in front of the camera, corrected when a projected
 * point moves by more than 0.01 px. A near-clipped outline's points do not
 * pair with the other's, so then the world points are compared.
 * @param raw The authored outline, near-clipped.
 * @param rn Its point count.
 * @param rclip 1 when the near plane cut raw.
 * @param poly The corrected outline, near-clipped.
 * @param pn Its point count.
 * @param clipped 1 when the near plane cut poly.
 * @param Wr The authored world outline.
 * @param Wn The corrected world outline.
 * @param wn The world point count.
 * @return 1 when corrected, else 0. */
static int portal_outline_corrected(const ppt* raw, int rn, int rclip, const ppt* poly, int pn, int clipped,
                                    const double (*Wr)[3], const double (*Wn)[3], int wn) {
    if (!clipped && !rclip) {
        if (rn != pn) return 0;
        for (int i = 0; i < pn; i++)
            if (fabsf(raw[i].x - poly[i].x) > 0.01f || fabsf(raw[i].y - poly[i].y) > 0.01f) return 1;
        return 0;
    }
    return memcmp(Wr, Wn, sizeof(double) * 3u * (size_t)wn) != 0;
}

/** @brief Marks the correction band, authored edge to corrected edge, behind the opening plane.
 *
 * Each edge's band is one world quad clipped at the near plane
 * (portal_clip_near) and fanned; wholly in front it projects to the four
 * outline points, two halves. Every vertex sits at its own view Z plus
 * s_sky_push_vz + portal_band_back_vz(), so stone in the band wins and the
 * terrain visible through a true gap loses.
 * @param Wr The authored world outline.
 * @param Wn The corrected world outline.
 * @param wn The world point count. */
static void emit_correction_band(const double (*Wr)[3], const double (*Wn)[3], int wn) OP_REQUIRES(sim) {
    for (int i = 0; i < wn; i++) {
        const int j = (i + 1) % wn;
        const double* wq[4] = { Wr[i], Wr[j], Wn[j], Wn[i] };
        double vq[4][3];
        for (int k = 0; k < 4; k++) portal_view_point(&s_view, wq[k], vq[k]);
        ppt q[8];
        const int qn = portal_clip_near(&s_view, (const double (*)[3])vq, 4, q, 8, NULL);
        for (int f = 1; f + 1 < qn; f++) {
            const int ks[3] = { 0, f, f + 1 };
            openpete_scene_tri t;
            memset(&t, 0, sizeof t);
            for (int k = 0; k < 3; k++) {
                const ppt* v = &q[ks[k]];
                t.v[k].sx = v->x; t.v[k].sy = v->y;
                t.v[k].z = openpete_native_scene_viewz(
                               openpete_worldvz_full((float)(v->vz + s_sky_push_vz + portal_band_back_vz())));
                t.v[k].rgba = 0xFF000000u;
            }
            portal_add_tri(&t, OPENPETE_SCENE_PORTAL_MASK);
        }
    }
}

/** @brief Marks the portal's stencil aperture from its opening polygon and sets s_aperture_ok when it does.
 *
 * The interior fan is the authored outline (portal_outline_clipped with no
 * correction, on a corrected portal, else poly) at the opening plane's
 * depth, eroded inward by |margin| when portal_margin_for is negative. A
 * positive margin adds an outward ring around poly behind the plane
 * (emit_aperture_ring); a corrected portal adds the band between the
 * authored outline and poly behind the plane, each edge's band quad clipped
 * at the near plane as its own world quad. Marks nothing when the aperture
 * is disabled, pn lies outside 3..PORTAL_POLY_MAX, or RP_PORTAL is filtered
 * out.
 * @param cpu The guest CPU state.
 * @param portal The Portal* guest address, or 0 when there is none.
 * @param poly The projected (corrected) outline, near-clipped.
 * @param pn The point count.
 * @param clipped 1 when the near plane cut poly.
 * @param use_plane Nonzero when pa, pb, pc hold the opening plane; else it is fitted from poly.
 * @param pa The plane's inverse-depth x coefficient.
 * @param pb The plane's inverse-depth y coefficient.
 * @param pc The plane's inverse-depth constant. */
static void emit_portal_aperture(CPUState* cpu, uint32_t portal, const ppt* poly, int pn, int clipped,
                                 int use_plane, double pa, double pb, double pc) OP_REQUIRES(sim) {
    s_aperture_ok = 0;
    if (!portal_aperture_enabled() || pn < 3 || pn > PORTAL_POLY_MAX) return;
    if (!portal_scene_emit_enabled()) return;
    /* Split the aperture at the AUTHORED opening.
     *
     * Interior (authored outline) is marked at plane depth, exactly as before
     * any correction existed. The strip between the authored opening and a
     * corrected one is marked BEHIND the plane instead: that band is part stone
     * and part genuine gap, and the preview must lose to the stone while still
     * beating the terrain visible through the shortfall. On the plane it is
     * coplanar with the stone and the depth test has no winner - the z-fighting
     * a corrected portal shows, and the reason no single sky bias resolves it.
     *
     * The interior must NOT be widened to the corrected outline: stencil
     * marking is a union, so a widened interior marks the band at plane depth
     * and the behind-depth strip underneath it becomes dead geometry. */
    ppt raw[PORTAL_POLY_MAX];
    int rclip = 0;
    const int rn = portal ? portal_outline_clipped(cpu, &s_view, portal, 0, raw, PORTAL_POLY_MAX, &rclip) : 0;
    double Wr[16][3], Wn[16][3];
    const int wn = (rn >= 3) ? portal_outline_world(cpu, portal, Wr, 0) : 0;
    const int corrected = wn && portal_outline_world(cpu, portal, Wn, 1) == wn &&
        portal_outline_corrected(raw, rn, rclip, poly, pn, clipped,
                                 (const double (*)[3])Wr, (const double (*)[3])Wn, wn);
    const ppt* interior = corrected ? raw : poly;   /* uncorrected = bit-identical */
    const int in_n = corrected ? rn : pn;
    const double margin = portal_margin_for(cpu, portal);
    double mpx[PORTAL_POLY_MAX], merode[PORTAL_POLY_MAX];
    margin_px_per_vertex(cpu, &s_view, interior, in_n, margin, mpx);
    const double msign = (margin < 0.0) ? -1.0 : 0.0;                         /* inward */
    for (int i = 0; i < in_n; i++) merode[i] = msign * mpx[i];
    float ex[PORTAL_POLY_MAX], ey[PORTAL_POLY_MAX];
    outline_offset(interior, in_n, merode, ex, ey);
    /* The outline is planar, so fit the plane here when the caller had none. */
    if (!use_plane) use_plane = fit_invz(poly, pn, &pa, &pb, &pc);
    float cz = 0.0f;
    const int cmode = sky_const_depth(&cz);
    for (int i = 1; i + 1 < in_n; i++) {                    /* fan-triangulate */
        openpete_scene_tri t;
        memset(&t, 0, sizeof t);
        t.textured = 0; t.blend = 0;
        const int idx3[3] = { 0, i, i + 1 };
        for (int k = 0; k < 3; k++) {
            const int j = idx3[k];
            const float pz = use_plane ? plane_depth(pa, pb, pc, ex[j], ey[j])
                                       : portal_cyclorama_depth(interior[j].vz);
            t.v[k].sx = ex[j]; t.v[k].sy = ey[j];
            t.v[k].z = sky_min2(pz, cz, cmode);
            t.v[k].rgba = 0xFF000000u;                      /* colour-write is off */
        }
        portal_add_tri(&t, OPENPETE_SCENE_PORTAL_MASK);
    }
    /* The margin ring belongs on the OUTERMOST boundary, which on a corrected
     * portal is the corrected outline, not the authored one. Emitted around the
     * interior it lands inside the correction band and does nothing at all -
     * the margin silently stopped working on exactly the arches whose openings
     * had to be moved, which is where it was needed most. */
    if (margin > 0.0)
    {   double rpx[PORTAL_POLY_MAX];
        margin_px_per_vertex(cpu, &s_view, poly, pn, margin, rpx);
        emit_aperture_ring(poly, pn, rpx, 0xFF000000u, OPENPETE_SCENE_PORTAL_MASK);
    }
    if (corrected) emit_correction_band((const double (*)[3])Wr, (const double (*)[3])Wn, wn);
    s_aperture_ok = 1;
}

/* use_plane: 0 = const_depth, 1 = the opening plane, 2 = that plane pushed
 * BEHIND it (plane_depth_back - the spill outside the opening). */
static void fan_emit_poly(const pgv* v, int n, float const_depth,
                          int use_plane, double pa, double pb, double pc) OP_REQUIRES(sim);

/* TEMP band census: 0=off, else tint each band producer a flat colour. */
static int s_band_dbg = -1;
static uint32_t s_band_tint = 0;   /* nonzero overrides fan_emit_poly's gouraud */
/** @brief Reports whether OPENPETE_PORTAL_BAND_DBG tints the aperture flat fan and ring.
 *
 * When set, the interior fan draws in blue and the ring in red. The
 * variable is read once and cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int band_dbg(void) {
    if (s_band_dbg < 0) { const char* e = getenv("OPENPETE_PORTAL_BAND_DBG"); s_band_dbg = (e && *e && *e != '0'); }
    return s_band_dbg;
}

/** @brief Fan-emits a clipped dome face unsplit through fan_emit_poly.
 *
 * The dome's confinement to the opening is the stencil mask alone. The
 * mask's admission depth discriminates per pixel: the interior fan marks at
 * plane depth, the dilation ring and correction band at their behind-plane
 * skirt depth, so a marked pixel is either the opening or a through-gap
 * (stone depth-fails the skirt and stays unmarked). Painting every marked
 * pixel at plane depth is therefore safe and makes the dome cover exactly
 * the pixels the mask and the flat base fan cover, with one boundary
 * rasterization. A geometric inside/outside split with float clips would
 * rasterize a second boundary, and a one-pixel disagreement shows the flat
 * base as a rim around the dome.
 * @param poly The face polygon.
 * @param n Its vertex count.
 * @param depth The constant depth when no plane is used.
 * @param use_plane 0 = depth, 1 = the opening plane, 2 = the plane pushed behind.
 * @param pa The plane fit's x coefficient.
 * @param pb The plane fit's y coefficient.
 * @param pc The plane fit's constant. */
static void emit_split_by_outline(const pgv* poly, int n, float depth,
                                  int use_plane, double pa, double pb, double pc) OP_REQUIRES(sim) {
    fan_emit_poly(poly, n, depth, use_plane, pa, pb, pc);
}

/** @brief Fan-triangulates a gouraud polygon and adds it as portal sky triangles.
 *
 * Each vertex's depth is chosen by use_plane, then combined with the
 * sky_const_depth mode through sky_min2. The colour is the vertex's
 * rounded, 255-clamped gouraud colour, or s_band_tint when set.
 * @param v The polygon.
 * @param n Its vertex count.
 * @param const_depth The depth when use_plane is 0.
 * @param use_plane 0 = const_depth, 1 = plane_depth, 2 = plane_depth_back.
 * @param pa The plane fit's x coefficient.
 * @param pb The plane fit's y coefficient.
 * @param pc The plane fit's constant. */
static void fan_emit_poly(const pgv* v, int n, float const_depth,
                          int use_plane, double pa, double pb, double pc) OP_REQUIRES(sim) {
    for (int i = 1; i + 1 < n; i++) {                       /* fan-triangulate */
        openpete_scene_tri t = {0};
        memset(&t, 0, sizeof t);
        t.textured = 0; t.blend = 0;
        const pgv* tv[3] = { &v[0], &v[i], &v[i + 1] };
        for (int k = 0; k < 3; k++) {
            uint32_t r = (uint32_t)(tv[k]->r + 0.5f), g = (uint32_t)(tv[k]->g + 0.5f), bl = (uint32_t)(tv[k]->b + 0.5f);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (bl > 255) bl = 255;
            float cz2 = 0.0f;
            int cm2 = sky_const_depth(&cz2);
            float pz2 = (use_plane == 2) ? plane_depth_back(pa, pb, pc, tv[k]->x, tv[k]->y)
                      : use_plane        ? plane_depth(pa, pb, pc, tv[k]->x, tv[k]->y)
                                         : const_depth;
            t.v[k].sx = tv[k]->x; t.v[k].sy = tv[k]->y;
            t.v[k].z = sky_min2(pz2, cz2, cm2);
            t.v[k].rgba = s_band_tint ? s_band_tint
                                      : (r | (g << 8) | (bl << 16) | 0xFF000000u);   /* low byte=R, matches cyc */
        }
        portal_add_sky_tri(&t);
    }
}

/** @brief Clips a gouraud dome face to the portal clip rect and emits it.
 *
 * This is what the dome leaves do: func_80050BD0 sets D_80077EA0[0] = 0, so
 * func_8005060C skips outline clipping and the dome is confined only by the
 * GPU scissor rect (D_8007AA00). The opening's shape comes from the stencil
 * aperture.
 * @param v0 The first vertex.
 * @param v1 The second vertex.
 * @param v2 The third vertex.
 * @param x0 The rect's minimum X.
 * @param y0 The rect's minimum Y.
 * @param x1 The rect's maximum X.
 * @param y1 The rect's maximum Y.
 * @param depth The constant depth when no plane is used.
 * @param use_plane 0 = depth, 1 = the opening plane, 2 = the plane pushed behind.
 * @param pa The plane fit's x coefficient.
 * @param pb The plane fit's y coefficient.
 * @param pc The plane fit's constant. */
static void emit_clipped_gouraud(pgv v0, pgv v1, pgv v2, int x0, int y0, int x1, int y1,
                                 float depth, int use_plane, double pa, double pb, double pc) OP_REQUIRES(sim) {
    pgv a[10], b[10];
    a[0] = v0; a[1] = v1; a[2] = v2;
    int n = 3;
    n = clip_half(a, n, b, 0, (float)x0, 1); if (n < 3) return;
    n = clip_half(b, n, a, 0, (float)x1, 0); if (n < 3) return;
    n = clip_half(a, n, b, 1, (float)y0, 1); if (n < 3) return;
    n = clip_half(b, n, a, 1, (float)y1, 0); if (n < 3) return;
    emit_split_by_outline(a, n, depth, use_plane, pa, pb, pc);
}

/* A collected dome face awaiting emission. The dome is drawn at the
 * single portal-plane depth so the arch occludes it; with every face at the
 * same depth the paint ORDER decides each pixel. The whole dome lands in ONE
 * OT bucket on the PSX (a single leaf depth - see the portal-dump OTdepth), so
 * the authored face-list order, not view-Z, is the paint order: a far->near
 * view-Z sort buries faces the .s paints over others (the beast_makers swamp
 * trunks project farther than the cloud band they are painted over). The
 * faces are emitted in walk order, index 0 first; with LESS_OR_EQUAL the last
 * emitted face wins per pixel. */
typedef struct { pgv a, b, c; double key; } dome_face;

/** @brief Feeds a destination-sky dome into the scene as gouraud sky triangles.
 *
 * Reads the clip rect from D_CLIPRECT and the dome rotation from GTE
 * control registers 0 to 4, which the leaf left loaded. Settles the OT depth
 * s_ot_vz and the sky push, then takes the opening's depth from the owning
 * portal's outline plane fit or its analytic plane, or a constant depth
 * when neither exists. The clip rect is widened to the near-clipped
 * outline's bounding box (portal_outline_rect), or, with no outline in front
 * of the camera, mapped onto an extended view (portal_fallback_rect); the
 * aperture is emitted, and up to 1024 sectors are walked (the
 * sector cull is skipped in an extended view). A face with a vertex at or
 * behind the near plane is dropped; colours are blended toward fog_rgb by
 * fog_t. Faces are emitted in walk order, clipped to the rect. Reads
 * gamestate only.
 * @param cpu The guest CPU state.
 * @param skybox The Skybox* guest address; sector count at +0x00, sector
 *     table at +0x04.
 * @param ot_b_hint The captured OT bucket for a replay, used when canonical
 *     is 0; 0 derives it from the outline.
 * @param canonical 1 to read the bucket from D_PORTAL_DEPTH.
 * @param fog_rgb The fog target colour, 0xBBGGRR.
 * @param fog_t The fog blend factor, 0 for none.
 * @return 1 after emitting, 0 when OPENPETE_PORTAL_REPR suppresses the dome
 *         or the clip rect is empty. */
static int feed_portal_dome(CPUState* cpu, uint32_t skybox, uint32_t ot_b_hint,
                            int canonical, uint32_t fog_rgb, double fog_t) OP_REQUIRES(sim) {
    if (portal_repr_skip(1)) return 0;
    s_portal_group = (int)skybox;   /* group id for mask<->colour pairing */
    int y0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x0) >> 16, y1 = (int32_t)cpu->read_word(D_CLIPRECT + 0x4) >> 16;
    int x0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x8) >> 16, x1 = (int32_t)cpu->read_word(D_CLIPRECT + 0xC) >> 16;
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    if (x1 <= x0 || y1 <= y0) return 0;

    /* dome rotation matrix the leaf left in the GTE (post-call), TR=0 (sky) */
    int M[3][3];
    ngte_unpack_matrix(cpu->gte_ctrl[0], cpu->gte_ctrl[1], cpu->gte_ctrl[2],
                       cpu->gte_ctrl[3], cpu->gte_ctrl[4], M);
    const double ofx = s_view.ofx, ofy = s_view.ofy, H = s_view.H_eff;

    if (canonical)
        s_ot_vz = 128.0 * (double)(int32_t)cpu->read_word(D_PORTAL_DEPTH);
    else if (ot_b_hint)
        s_ot_vz = 128.0 * (double)ot_b_hint;
    else
        s_ot_vz = 0.0;
    double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
    double vz = portal_viewz(cpu, &s_view, skybox, cx, cy);
    int crossed = (vz <= 1.0);
    if (crossed) vz = 2.0;     /* camera crossed the plane -> nearest (see feed_portal_quad) */

    /* Tilted-plane window: outline polygon + its perspective-correct inv-vz depth
     * field, so each dome pixel takes the portal-plane depth (the stone frame
     * occludes it into the arch shape even at a sharp angle, f2945). The outline
     * is clipped at the near plane; the rect clip + constant depth remain only
     * when the whole outline is behind the camera. */
    uint32_t p = owning_portal_ptr(cpu, &s_view, skybox, cx, cy);
    ppt poly[PORTAL_POLY_MAX];
    int clipped = 0;
    int pn = p ? portal_outline_clipped(cpu, &s_view, p, 1, poly, PORTAL_POLY_MAX, &clipped) : 0;
    /* A revealed (culled) portal has no live/captured bucket - the outline
     * mirror (validated == live) supplies it so the push agrees with the
     * flat fan the reveal already emitted. */
    if (s_ot_vz <= 0.0 && pn >= 3) set_ot_sky_vz(cpu, &s_view, p);
    if (pn >= 3) {
        portal_clip_note(PORTAL_CLIP_FRONT + (clipped != 0));
    } else {
        /* No outline in front of the camera: the rect alone bounds the dome. */
        portal_fallback_rect(&s_view, &x0, &y0, &x1, &y1);
    }
    set_sky_ot_push();
    if (crossed) s_sky_push_vz = 0.0;   /* mid-entry cover must win everything */
    float depth = portal_cyclorama_depth(vz);   /* rect-fallback depth */
    {
        float cz3 = 0.0f;
        int cm3 = sky_const_depth(&cz3);
        if (!crossed && cm3) depth = sky_min2(depth, cz3, cm3);
    }
    double pa = 0, pb = 0, pc = 0;
    int use_plane = (pn >= 3) && fit_invz(poly, pn, &pa, &pb, &pc);
    /* Analytic plane when the projected fit is unavailable (a point behind
     * the camera - the common close/steep case): per-pixel plane depth
     * instead of the constant rect fallback, so the floor meets the
     * opening's near edge instead of eating it. No outline registration
     * (s_skyout stays 0): the whole dome sits at plane depth incl. the rect
     * spill, which now only beats stone within the O(16-unit) sky margin. */
    if (!use_plane && !crossed && p && portal_plane_invz(cpu, &s_view, p, &pa, &pb, &pc))
        use_plane = 1;
    /* Register the outline for the two-region sky-depth split (see
     * emit_split_by_outline): orient CCW by signed area. */
    s_skyout_n = 0;
    if (use_plane && pn >= 3) register_skyout(poly, pn);

    /* Widescreen un-clamp: the .s clip rect D_8007AA00 is the GPU scissor, clamped
     * to the 4:3 framebuffer [0,512]. For a portal at the screen edge the outline
     * (and the stone arch, which widens through scene_to_clip) extends past 512,
     * but the dome - clipped to the clamped rect - gets cut at the 4:3 boundary,
     * leaving the widened arch's outer part with no sky. Extend the rect to the
     * projected-outline bbox (still a RECT clip, so depth occlusion still shapes
     * the arch - no coverage gaps; UNION so a non-clamped 4:3 rect is unchanged ->
     * no 4:3 regression, and at 4:3 any spill past 512 is GPU-scissored anyway). */
    if (pn >= 3) {
        int orc[4];
        portal_outline_rect(&s_view, poly, pn, clipped, 0, orc);
        /* This rect is NOT widened by the aperture margin. Tried, and it made
         * no difference on any of the 29 portals - the mask reaches past the
         * outline but the colours filling that band are not bounded by this
         * rect, so tightening or loosening it changes nothing. Don't re-add. */
        if (orc[2] < x0) x0 = orc[2];
        if (orc[3] > x1) x1 = orc[3];
        if (orc[0] < y0) y0 = orc[0];
        if (orc[1] > y1) y1 = orc[1];
    }
    emit_portal_aperture(cpu, p, poly, pn, clipped, use_plane, pa, pb, pc);

    uint32_t cnt = cpu->read_word(skybox + 0x00), arr = cpu->read_word(skybox + 0x04);
    if (cnt > 1024) cnt = 1024;
    static float  px[4096], py[4096];
    static double pz[4096];
    static uint32_t col[4096];
    static dome_face faces[8192]; int nf = 0;   /* collected for far->near depth sort */
    for (uint32_t si = 0; si < cnt; si++) {
        uint32_t S = cpu->read_word(arr + si * 4);
        if (!S) continue;
        /* phase-1 cull: project the sector's representative point, skip if behind
         * the cull radius (same test as the main cyclorama). The test is
         * against the DOME-FORWARD axis - correct for the 4:3 window (±26°
         * azimuth) but an off-4:3 widescreen portal peers at the dome up to
         * ±60°+ off forward, where needed sectors cross the boundary as the
         * dome spins (segments missing as it rotates, flashing the flat
         * backstop). Under Hor+ skip the sector cull - the per-face
         * near-plane reject + the arch clip rect still bound the work. */
        uint32_t c0 = cpu->read_word(S + 0x00), c1 = cpu->read_word(S + 0x04);
        int cvx = (int16_t)(c0 & 0xFFFF), cvy = (int16_t)(c0 >> 16), cvz = (int16_t)(c1 >> 16), radius = (int16_t)(c1 & 0xFFFF);
        double cm3 = (M[2][0]*(double)cvx + M[2][1]*(double)cvy + M[2][2]*(double)cvz) / 4096.0;
        if (!s_view.extended && cm3 <= (double)radius) continue;

        uint32_t h8 = cpu->read_word(S + 0x08), hC = cpu->read_word(S + 0x0C), hF = cpu->read_word(S + 0x10);
        int centerX = (int16_t)(h8 & 0xFFFF), centerY = (int16_t)(h8 >> 16), centerZ = (int16_t)(hC >> 16);
        uint32_t vc = hC & 0xFFFF, fc = hF & 0x1FFF, foff = hF >> 14;
        if (vc > 4096) vc = 4096;
        uint32_t vertBase = S + 0x18, colorBase = vertBase + vc * 4, faceBase = colorBase + foff;
        for (uint32_t i = 0; i < vc; i++) {
            uint32_t w = cpu->read_word(vertBase + i * 4);
            int vx = centerX - (int)((w >> 10) & 0x7FF), vy = centerY - (int)(w & 0x3FF), vz2 = centerZ + (int)(w >> 21);
            double m1, m2, m3;
            ngte_mac(M, (double)vx, (double)vy, (double)vz2, &m1, &m2, &m3);
            pz[i] = m3; double z = (m3 > 1.0) ? m3 : 1.0;
            px[i] = (float)(ofx + m1 * H / z); py[i] = (float)(ofy + m2 * H / z);
        }
        uint32_t colorCount = foff / 4; if (colorCount > 4096) colorCount = 4096;
        for (uint32_t j = 0; j < colorCount; j++) col[j] = cpu->read_word(colorBase + j * 4) & 0xFFFFFF;
        for (uint32_t f = 0; f < fc; f++) {
            uint32_t fw0 = cpu->read_word(faceBase + f * 8), fw1 = cpu->read_word(faceBase + f * 8 + 4);
            uint32_t i0 = (fw0 >> 22) & 0x3FF, i1 = (fw0 >> 12) & 0xFF, i2 = (fw0 >> 2) & 0xFF;
            uint32_t k0 = (fw1 >> 22) & 0x3FF, k1 = (fw1 >> 12) & 0xFF, k2 = (fw1 >> 2) & 0xFF;
            if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
            if (pz[i0] <= 1.0 || pz[i1] <= 1.0 || pz[i2] <= 1.0) continue;   /* near-plane reject */
            uint32_t cc0 = (k0 < colorCount) ? col[k0] : 0, cc1 = (k1 < colorCount) ? col[k1] : 0, cc2 = (k2 < colorCount) ? col[k2] : 0;
            /* blended-medium dome (func_80050240): DPCS depth-cue fades each vertex
             * colour toward the fog target (fog_t=0 for the un-fogged F4BC dome). */
            cc0 = blend_to_fog(cc0, fog_rgb, fog_t);
            cc1 = blend_to_fog(cc1, fog_rgb, fog_t);
            cc2 = blend_to_fog(cc2, fog_rgb, fog_t);
            if (nf >= (int)(sizeof faces / sizeof faces[0])) continue;
            dome_face* df = &faces[nf++];
            df->a.x = px[i0]; df->a.y = py[i0]; df->a.r = cc0 & 0xFF; df->a.g = (cc0 >> 8) & 0xFF; df->a.b = (cc0 >> 16) & 0xFF;
            df->b.x = px[i1]; df->b.y = py[i1]; df->b.r = cc1 & 0xFF; df->b.g = (cc1 >> 8) & 0xFF; df->b.b = (cc1 >> 16) & 0xFF;
            df->c.x = px[i2]; df->c.y = py[i2]; df->c.r = cc2 & 0xFF; df->c.g = (cc2 >> 8) & 0xFF; df->c.b = (cc2 >> 16) & 0xFF;
            df->key = (pz[i0] + pz[i1] + pz[i2]) / 3.0;   /* face view-Z for the far->near sort */
        }
    }
    /* Emit in walk order (see the dome_face comment). */
    for (int i = 0; i < nf; i++)
        emit_clipped_gouraud(faces[i].a, faces[i].b, faces[i].c, x0, y0, x1, y1, depth, use_plane, pa, pb, pc);
    {   /* OPENPETE_PORTAL_DOME_DBG=1: per-call walk census (throttled) */
        static int dd = -1; static int lines;
        if (dd < 0) { const char* e = getenv("OPENPETE_PORTAL_DOME_DBG"); dd = (e && *e && *e != '0'); }
        static unsigned dfrom = 0;
        if (dd && !dfrom) { const char* e2 = getenv("OPENPETE_PORTAL_DOME_DBG_FROM"); dfrom = e2 ? (unsigned)atoi(e2) : 0; }
        if (dd && lines < 40 && openpete_frame >= dfrom) {
            lines++;
            float fx0 = 1e9f, fx1 = -1e9f, fy0 = 1e9f, fy1 = -1e9f;
            int inrect = 0;
            for (int i2 = 0; i2 < nf; i2++) {
                const dome_face* f2 = &faces[i2];
                float bx0 = f2->a.x, bx1 = f2->a.x, by0 = f2->a.y, by1 = f2->a.y;
                if (f2->b.x < bx0) bx0 = f2->b.x;
                if (f2->b.x > bx1) bx1 = f2->b.x;
                if (f2->c.x < bx0) bx0 = f2->c.x;
                if (f2->c.x > bx1) bx1 = f2->c.x;
                if (f2->b.y < by0) by0 = f2->b.y;
                if (f2->b.y > by1) by1 = f2->b.y;
                if (f2->c.y < by0) by0 = f2->c.y;
                if (f2->c.y > by1) by1 = f2->c.y;
                if (bx0 < fx0) fx0 = bx0;
                if (bx1 > fx1) fx1 = bx1;
                if (by0 < fy0) fy0 = by0;
                if (by1 > fy1) fy1 = by1;
                if (bx1 >= x0 && bx0 <= x1 && by1 >= y0 && by0 <= y1) inrect++;
            }
            fprintf(stderr, "[portal-domedbg f=%u] sb=%08X sectors=%u faces=%d (inrect=%d, span x[%.0f,%.0f] y[%.0f,%.0f]) "
                            "rect x[%d,%d] y[%d,%d] plane=%d vz=%.0f\n",
                    openpete_frame, skybox, cnt, nf, inrect, fx0, fx1, fy0, fy1,
                    x0, x1, y0, y1, use_plane, vz);
        }
    }
    return 1;
}

/** @brief Prints one portal leaf call to stderr.
 *
 * Prints the clip rect, the colour at a0[0] (the flat paths' colour), the
 * OT depth, the owning portal's matched view-Z and its native depth, the
 * polybuf byte count, and the fog colour and factor; then the owning
 * portal's projected outline bounding box beside the rect and the view's
 * horizontal bounds. For the dome leaves (labels beginning "F4" or "5") it
 * adds the GTE state the leaf left, the skybox sector table, sector 0's
 * header and first vertices, and the first ten polybuf words.
 * @param cpu The guest CPU state.
 * @param which The leaf's label.
 * @param a0 The leaf's a0.
 * @param a1 Unused.
 * @param a2 Unused.
 * @param start The polybuf cursor before the call.
 * @param end The polybuf cursor after the call. */
static void dump_leaf(CPUState* cpu, const char* which, uint32_t a0, uint32_t a1,
                      uint32_t a2, uint32_t start, uint32_t end) {
    int ry0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x0) >> 16, ry1 = (int32_t)cpu->read_word(D_CLIPRECT + 0x4) >> 16;
    int rx0 = (int32_t)cpu->read_word(D_CLIPRECT + 0x8) >> 16, rx1 = (int32_t)cpu->read_word(D_CLIPRECT + 0xC) >> 16;
    uint32_t col = (a0 >= 0x80010000u && a0 < 0x80200000u) ? (cpu->read_word(a0) & 0xFFFFFF) : 0xFFFFFFFFu;
    double vz = portal_viewz(cpu, &s_view, a0, (rx0 + rx1) * 0.5, (ry0 + ry1) * 0.5);
    uint32_t fog = cpu->read_word(game_layout.fog.far_colour) & 0xFFFFFF, ir0 = cpu->read_word(game_layout.fog.factor) & 0xFFFF;
    fprintf(stderr, "[portal-dump f=%u] %s a0=%08X colour=%06X clip x[%d,%d] y[%d,%d] "
                    "OTdepth=%d matchedViewZ=%.0f nativeZ=%.3f  prims=%u bytes  fogFC=%06X IR0=%u(t=%.3f)\n",
            openpete_frame, which, a0, col, rx0, rx1, ry0, ry1,
            (int32_t)cpu->read_word(D_PORTAL_DEPTH), vz,
            (double)openpete_native_scene_viewz(openpete_worldvz_full((float)(vz > 1.0 ? vz : 16384.0))),
            (end > start) ? (end - start) : 0, fog, ir0, (double)ir0 / 4096.0);
    /* Widescreen bug probe: the CLAMPED .s scissor rect (above) vs the native
     * projected-outline bbox (which widens through scene_to_clip). If the outline
     * X extends past the rect's clamped edge, the dome/flat-fallback (clipped to
     * the rect) is cut at the 4:3 boundary while the stone arch widens -> mismatch. */
    {
        uint32_t pp = owning_portal_ptr(cpu, &s_view, a0, (rx0 + rx1) * 0.5, (ry0 + ry1) * 0.5);
        ppt poly[16]; int pn = pp ? portal_outline_screen(cpu, &s_view, pp, poly, 16) : 0;
        if (pn >= 3) {
            float oxlo = poly[0].x, oxhi = poly[0].x, oylo = poly[0].y, oyhi = poly[0].y;
            for (int i = 1; i < pn; i++) {
                if (poly[i].x < oxlo) oxlo = poly[i].x;
                if (poly[i].x > oxhi) oxhi = poly[i].x;
                if (poly[i].y < oylo) oylo = poly[i].y;
                if (poly[i].y > oyhi) oyhi = poly[i].y;
            }
            fprintf(stderr, "    outline-bbox x[%.0f,%.0f] y[%.0f,%.0f] (rect x[%d,%d]) wsF=%.3f visX[%.0f,%.0f]\n",
                    oxlo, oxhi, oylo, oyhi, rx0, rx1,
                    s_view.F, s_view.xlo, s_view.xhi);
        }
    }
    /* For the dome leaf (F4BC, a0=skybox), capture everything the float walk
     * needs: the GTE transform it projected with (post-call), the skybox sector
     * table, sector0's cyclorama-format header, and the first emitted prims. */
    if (((which[0] == 'F' && which[1] == '4') || (which[0] == '5')) && a0 >= 0x80010000u && a0 < 0x80200000u) {
        fprintf(stderr, "    GTE post: M=%08X %08X %08X %08X %08X TR=%d,%d,%d OFX=%d OFY=%d H=%u\n",
                cpu->gte_ctrl[0], cpu->gte_ctrl[1], cpu->gte_ctrl[2], cpu->gte_ctrl[3], cpu->gte_ctrl[4],
                (int32_t)cpu->gte_ctrl[5], (int32_t)cpu->gte_ctrl[6], (int32_t)cpu->gte_ctrl[7],
                (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16, cpu->gte_ctrl[26] & 0xFFFF);
        uint32_t cnt = cpu->read_word(a0 + 0x00), arr = cpu->read_word(a0 + 0x04);
        uint32_t s0 = cpu->read_word(arr);
        fprintf(stderr, "    skybox cnt=%u sectorArr=%08X sec0=%08X  sec0 hdr:", cnt, arr, s0);
        for (int o = 0; o <= 0x14; o += 4) fprintf(stderr, " [%02X]=%08X", o, cpu->read_word(s0 + o));
        fprintf(stderr, "\n    sec0 verts@+18:");
        for (int i = 0; i < 4; i++) fprintf(stderr, " %08X", cpu->read_word(s0 + 0x18 + i*4));
        fprintf(stderr, "\n    prims:");
        for (int i = 0; i < 10 && start + i*4 < end; i++) fprintf(stderr, " %08X", cpu->read_word(start + i*4));
        fprintf(stderr, "\n");
    }
    (void)a1; (void)a2;
}

static void derive_dome_matrix_at(CPUState* cpu, double yaw, double pitch, int M[3][3]);
static void derive_dome_matrix_cam(const double cam[3], double yaw, double pitch, int M[3][3]);
/** @brief Native derivation of the shared portal-dome rotation matrix of RenderPortals.
 *
 * The func_80050BD0 prologue computes it once per tick on its STACK, unreadable
 * at leaf-hook time, so it is mirrored: M = RotX(cam.pitch(+0x4E) −
 * D_PORTAL_PITCH) × RotY(cam.head(+0x50) + D_PORTAL_YAW) ×
 * RotZ(cam.roll(+0x4C)), game angle units 4096/circle, Q12 components rounded
 * to nearest. RotX's middle row carries g_Camera's Y-aspect factor 2560/4096.
 * Sin and cos are taken in double; the validation probe
 * (OPENPETE_PORTAL_MAT_DBG) measures the result against the matrix the .s
 * leaves in the GTE on ticks that draw the dome.
 * @param cpu guest CPU state (reads g_Camera, D_PORTAL_YAW, D_PORTAL_PITCH)
 * @param M   receives the matrix, Q12 */
static void derive_dome_matrix(CPUState* cpu, int M[3][3]) {
    derive_dome_matrix_at(cpu, (double)(int32_t)cpu->read_word(D_PORTAL_YAW),
                          (double)(int32_t)cpu->read_word(D_PORTAL_PITCH), M);
}

/** @brief derive_dome_matrix with the portal spin angles `yaw` and `pitch` given instead of read.
 *
 * `yaw` stands for D_PORTAL_YAW and `pitch` for D_PORTAL_PITCH, so a sub-tick
 * present can pass angles between two ticks' values.
 * @param cpu   guest CPU state (reads g_Camera's angles)
 * @param yaw   portal spin yaw, game angle units (4096/circle)
 * @param pitch portal spin pitch, game angle units (4096/circle)
 * @param M     receives the matrix, Q12 */
static void derive_dome_matrix_at(CPUState* cpu, double yaw, double pitch, int M[3][3]) {
    const double cam[3] = {
        (double)(int16_t)(cpu->read_word(G_CAMERA + 0x4Cu) & 0xFFFFu),   /* +0x4C roll    */
        (double)(int16_t)(cpu->read_word(G_CAMERA + 0x4Cu) >> 16),       /* +0x4E pitch   */
        (double)(int16_t)(cpu->read_word(G_CAMERA + 0x50u) & 0xFFFFu),   /* +0x50 heading */
    };
    derive_dome_matrix_cam(cam, yaw, pitch, M);
}

/** @brief derive_dome_matrix with g_Camera's rotation angles given instead of read.
 *
 * @param cam   g_Camera roll (+0x4C), pitch (+0x4E) and heading (+0x50), game
 *              angle units (4096/circle)
 * @param yaw   portal spin yaw, game angle units
 * @param pitch portal spin pitch, game angle units
 * @param M     receives the matrix, Q12 */
static void derive_dome_matrix_cam(const double cam[3], double yaw, double pitch, int M[3][3]) {
    double TAU = 6.283185307179586;
    /* Game angle units: 4096/circle (the game's Cos/Sin table domain). The
     * pitch matrix carries the camera convention's Y-ASPECT bake: its middle
     * ROW is scaled 2560/4096 = 5/8 (the same non-orthonormal structure as
     * g_Camera's matrix). */
    double a1 = (cam[1] - pitch) * TAU / 4096.0;   /* +0x4E − pitch */
    double a2 = (cam[2] + yaw) * TAU / 4096.0;     /* +0x50 + yaw   */
    double a0 = cam[0] * TAU / 4096.0;             /* +0x4C roll    */
    double c1 = cos(a1), s1 = sin(a1), c2 = cos(a2), s2 = sin(a2), c0 = cos(a0), s0 = sin(a0);
    double A = 2560.0 / 4096.0;   /* the Y-row aspect bake */
    double X[3][3] = {{1,0,0},{0,A*c1,-A*s1},{0,s1,c1}};
    double Y[3][3] = {{c2,0,s2},{0,1,0},{-s2,0,c2}};
    double Z[3][3] = {{c0,s0,0},{-s0,c0,0},{0,0,1}};
    double T[3][3], R[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        T[i][j] = X[i][0]*Y[0][j] + X[i][1]*Y[1][j] + X[i][2]*Y[2][j];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        R[i][j] = T[i][0]*Z[0][j] + T[i][1]*Z[1][j] + T[i][2]*Z[2][j];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        M[i][j] = (int)lround(R[i][j] * 4096.0);
}

#include "no_lod.h"   /* openpete_no_lod: --no-lod always renders the geometric dome */

/** @brief Reports whether --no-lod forces the geometric portal dome.
 *
 * On under openpete_no_lod unless OPENPETE_NOLOD_PORTAL_DOME starts with
 * '0'; the variable is read once and cached.
 * @return 1 when the dome is forced, else 0. */
static int nolod_portal_dome_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_NOLOD_PORTAL_DOME"); v = !(e && *e == '0'); }
    return openpete_no_lod && v;
}

/* How a native preview takes RenderPortals' far-leaf fog term. ORIGINAL is
 * the game's integer term at the integer camera; PER_PIXEL the same curve in
 * float at the present camera; OFF no term. */
typedef enum {
    PORTAL_FOG_ORIGINAL,
    PORTAL_FOG_PER_PIXEL,
    PORTAL_FOG_OFF,
} portal_fog_mode_t;

/** @brief The fog mode the forced dome and its backstop take the far-leaf term in: the distance-fog form (openpete_fog_mode).
 * @return PORTAL_FOG_ORIGINAL, PORTAL_FOG_PER_PIXEL or PORTAL_FOG_OFF. */
static portal_fog_mode_t portal_forced_dome_fog_mode(void) {
    switch (openpete_fog_mode()) {
    case OPENPETE_FOG_ORIGINAL: return PORTAL_FOG_ORIGINAL;
    case OPENPETE_FOG_OFF:      return PORTAL_FOG_OFF;
    default:                    return PORTAL_FOG_PER_PIXEL;
    }
}

/** @brief RenderPortals' far-leaf fog IR0 for portal `p`, the game's integer term at the view's integer camera.
 *
 * With d the portal_game_distance, d above 0x4000 gives
 * clamp((0x8000 - d) >> 2, 0, 0x1000), the IR0 of ColorLerp([g_Cyclorama+0x10],
 * flat, IR0) (0x80050FDC-0x80051028); d at or below 0x4000 gives 0x1000, the
 * flat unfaded.
 * @param cpu The guest CPU state.
 * @param v The view; supplies cam_ram.
 * @param p The Portal pointer.
 * @return IR0 in 0..0x1000; 0 is the fog colour. */
static int portal_far_fog_ir0_game(CPUState* cpu, const ngte_view* v, uint32_t p) {
    const int32_t d = portal_game_distance(cpu, v->cam_ram, p);
    if (d <= 0x4000) return 0x1000;
    const int32_t s2 = (0x8000 - d) >> 2;
    return s2 < 0 ? 0 : (s2 > 0x1000 ? 0x1000 : s2);
}

/** @brief The far-leaf fog IR0 for portal `p` in float, at the view's present camera.
 *
 * The curve of portal_far_fog_ir0_game with d the float distance from
 * v->cam (sub-tick interpolated when available) to m_Points[0] (p+0x20).
 * @param cpu The guest CPU state.
 * @param v The view; supplies cam.
 * @param p The Portal pointer.
 * @return IR0 in 0..4096; 0 is the fog colour. */
static double portal_far_fog_ir0_float(CPUState* cpu, const ngte_view* v, uint32_t p) {
    double sq = 0.0;
    for (int k = 0; k < 3; k++) {
        const double e = (double)(int32_t)cpu->read_word(p + 0x20u + 4u * (uint32_t)k) - v->cam[k];
        sq += e * e;
    }
    const double d = sqrt(sq);
    if (d <= 16384.0) return 4096.0;
    const double s2 = (32768.0 - d) * 0.25;
    return s2 < 0.0 ? 0.0 : (s2 > 4096.0 ? 4096.0 : s2);
}

/** @brief The blend fraction toward [g_Cyclorama+0x10] that RenderPortals' far-leaf term gives portal `p`.
 *
 * The term applies only while g_SkipLowPolyWorld is nonzero
 * (0x80050FC8-0x80050FD4); ColorLerp(fog, flat, IR0) moves the flat toward
 * the fog by (4096 - IR0) / 4096, which blend_to_fog applies to every
 * colour of a preview. The white pulse the game mixes into the flat first
 * is not part of the term.
 * @param cpu The guest CPU state.
 * @param v The view the IR0 is evaluated under.
 * @param p The Portal pointer.
 * @param mode Which form of the term to evaluate.
 * @param fog_rgb Receives [g_Cyclorama+0x10] as 0x00BBGGRR, 0 when the term is off.
 * @return The blend fraction in [0, 1]; 0 when the term does not apply. */
static double portal_far_fog_t(CPUState* cpu, const ngte_view* v, uint32_t p,
                               portal_fog_mode_t mode, uint32_t* fog_rgb) {
    *fog_rgb = 0;
    if (mode == PORTAL_FOG_OFF || !p || cpu->read_word(G_SKIP_LOWPOLY_WORLD) == 0) return 0.0;
    const double ir0 = (mode == PORTAL_FOG_ORIGINAL) ? (double)portal_far_fog_ir0_game(cpu, v, p)
                                                     : portal_far_fog_ir0_float(cpu, v, p);
    if (ir0 >= 4096.0) return 0.0;
    *fog_rgb = cpu->read_word(G_CYCLORAMA + 0x10u) & 0xFFFFFFu;
    return (4096.0 - ir0) / 4096.0;
}

/* ---- dispatch hooks ------------------------------------------------- */
/** @brief Reports whether the portal dump is enabled and openpete_frame has reached OPENPETE_PORTAL_FRAME.
 *
 * @return 1 when the leaf and portal-table dumps print, else 0. */
static int s_want_dump_this_frame(void) {
    return portal_dump_enabled() && openpete_frame >= portal_dump_frame();
}

/* Which native render a leaf maps to. The same destination skybox renders three
 * ways across a portal's approach: FLAT colour when far, the DPCS-fogged dome at
 * medium (the cross-dissolve), the full gouraud dome when close. */
#define PORTAL_FLAT     0   /* func_8004FEA0: flat colour (far preview / close backstop) */
#define PORTAL_DOME     1   /* func_8004F4BC: full gouraud dome, no fog                  */
#define PORTAL_BLENDED  2   /* func_80050240: dome with DPCS fog toward D_800757D4       */

/* Rect-rule census (OPENPETE_PORTAL_RECT_CHECK=1): leaf captures whose rect
 * the RenderPortals mirror reproduces under the canonical view, and the
 * misses, printed at exit; the first misses are logged with both rects. */
static unsigned s_rect_match, s_rect_miss;
/** @brief Print the rect-mirror census at exit. */
static void portal_rect_census(void) {
    OP_ROLE_ASSERT(sim);
    fprintf(stderr, "portal: rect mirror: match=%u miss=%u\n", s_rect_match, s_rect_miss);
}

/** @brief The rule a replay rebuilds a leaf capture's clip rect by.
 *
 * The RenderPortals mirror for the leaf's pass when it reproduces the captured
 * rect under the canonical view, else the captured rect. */
static int portal_rect_rule_for(CPUState* cpu, uint32_t owner, int kind, const int clip[4]) {
    static int chk = -1;
    if (chk < 0) {
        const char* e = getenv("OPENPETE_PORTAL_RECT_CHECK");
        chk = (e && *e && *e != '0');
        if (chk) atexit(portal_rect_census);
    }
    if (!owner) return PORTAL_RECT_CAPTURED;
    const int dome = (kind != PORTAL_FLAT);
    int mr[4];
    const int ok = portal_game_rect(cpu, &s_view, owner, dome, mr) &&
                   mr[0] == clip[0] && mr[1] == clip[1] && mr[2] == clip[2] && mr[3] == clip[3];
    if (ok) s_rect_match++; else s_rect_miss++;
    if (chk && !ok && s_rect_miss <= 20)
        fprintf(stderr, "[portal-rect f=%u] MISS p=%08X kind=%d captured y[%d,%d] x[%d,%d] "
                "mirror y[%d,%d] x[%d,%d]\n", openpete_frame, owner, kind,
                clip[0], clip[1], clip[3], clip[2], mr[0], mr[1], mr[3], mr[2]);
    if (!ok) return PORTAL_RECT_CAPTURED;
    return dome ? PORTAL_RECT_DOME : PORTAL_RECT_FLAT;
}

/* Generic leaf hook: run the real renderer, capturing the polybuf range, then
 * dump. A re-entry guard keeps a nested call (shouldn't happen for leaves)
 * from re-dumping. */
static int s_leaf_in = 0;
/* Portals a leaf actually rendered this frame (the on-4:3 set), so the widescreen
 * off-4:3 reveal (feed_revealed_portals) doesn't double-draw them. Reset per frame
 * in on_80050BE0 before the real RenderPortals runs. */
static uint32_t s_fed_portal[8]; static int s_fed_n = 0;
/** @brief Portal leaf hook body.
 *
 * Inert (returns 0) unless the portal dump, the native scene, or a render-path
 * selection is on, and on re-entry. Runs the leaf `real`, feeds its preview of
 * kind `kind` (PORTAL_FLAT, _DOME, _BLENDED) natively, records the fed portal
 * for the widescreen reveal, and captures the leaf's decision, clip rect and
 * rect rule for the sub-tick replay. Under --no-lod a far flat leaf (not a
 * close backstop) also feeds the geometric dome of its portal's skybox, the
 * dome and its backstop taking the far flat's fog term (portal_far_fog_t),
 * and a blended leaf is fed without fog.
 * @param cpu   guest CPU state
 * @param which the leaf's label in the portal dump
 * @param real  the leaf's recompiled function
 * @param kind  PORTAL_FLAT, PORTAL_DOME or PORTAL_BLENDED
 * @return 1 after running the leaf, 0 when inert or re-entered */
static int leaf_hook(CPUState* cpu, const char* which, void (*real)(CPUState*), int kind) OP_REQUIRES(sim) {
    int feed = openpete_native_scene_enabled();   /* --renderer vk live */
    if (!portal_dump_enabled() && !feed && !render_paths_active()) return 0; /* inert */
    if (s_leaf_in) return 0;
    s_leaf_in = 1;
    uint32_t a0 = cpu->a0, a1 = cpu->a1, a2 = cpu->a2;
    uint32_t start = cpu->read_word(POLYBUF_CURSOR);
    real(cpu);
    uint32_t end = cpu->read_word(POLYBUF_CURSOR);
    ngte_view_build(cpu, &s_view);   /* the GTE state the leaf left behind */
    static int s_reports = 0;
    if (s_want_dump_this_frame() && s_reports < 512) { dump_leaf(cpu, which, a0, a1, a2, start, end); s_reports++; }
    if (feed) {
        render_material_level_push(cpu);
        openpete_native_scene_begin_frame(openpete_frame);  /* append; cyclorama/world already began it */
        int is_dome = (kind != PORTAL_FLAT);
        /* Stamp the preview kind for the emits this leaf is about to make. */
        s_portal_kind = (kind == PORTAL_DOME)    ? OPENPETE_TAG_PORTAL_DOME
                      : (kind == PORTAL_BLENDED) ? OPENPETE_TAG_PORTAL_BLENDED
                                                 : OPENPETE_TAG_PORTAL_FLAT;
        /* TEMP: derived-vs-GTE dome matrix validation */
        { static int md=-1; if(md<0){const char*e=getenv("OPENPETE_PORTAL_MAT_DBG");md=(e&&*e&&*e!='0');}
          if (md && is_dome) { int M[3][3], D[3][3]; read_gte_rotmatrix(cpu, M);
            derive_dome_matrix(cpu, D);
            int worst=0; for(int i2=0;i2<3;i2++)for(int j2=0;j2<3;j2++){int d=M[i2][j2]-D[i2][j2];if(d<0)d=-d;if(d>worst)worst=d;}
            fprintf(stderr, "[pmat f=%u] gte=[%d %d %d|%d %d %d|%d %d %d] drv=[%d %d %d|%d %d %d|%d %d %d] worst=%d "
              "raw c4C=%d c4E=%d c50=%d yaw=%d pitch=%d\n",
              openpete_frame,
              M[0][0],M[0][1],M[0][2],M[1][0],M[1][1],M[1][2],M[2][0],M[2][1],M[2][2],
              D[0][0],D[0][1],D[0][2],D[1][0],D[1][1],D[1][2],D[2][0],D[2][1],D[2][2],worst,
              (int)(int16_t)(cpu->read_word(G_CAMERA+0x4Cu)&0xFFFFu),
              (int)(int16_t)(cpu->read_word(G_CAMERA+0x4Cu)>>16),
              (int)(int16_t)(cpu->read_word(G_CAMERA+0x50u)&0xFFFFu),
              (int)(int32_t)cpu->read_word(D_PORTAL_YAW),
              (int)(int32_t)cpu->read_word(D_PORTAL_PITCH)); } }
        /* The blended-medium leaf (func_80050240) is the cross-dissolve: it draws
         * the SAME dome as F4BC but runs DPCS depth-cue, fading every colour toward
         * the GTE far-colour FC (D_800757D4) by IR0/4096 (D_8007575C). Far = IR0≈4096
         * (faded fully to the flat colour, the FEA0 preview); close = IR0≈0 (full
         * dome). Apply the same fade to its backstop fill so the gaps match. */
        uint32_t fog = 0; double fog_t = 0.0;
        if (kind == PORTAL_BLENDED && !nolod_portal_dome_on()) {
            /* --no-lod: skip the medium-range cross-dissolve toward the flat
             * colour - the dome renders full-colour at any distance. */
            fog   = cpu->read_word(game_layout.fog.far_colour) & 0xFFFFFF;
            fog_t = (double)(cpu->read_word(game_layout.fog.factor) & 0xFFFF) / 4096.0;
            if (fog_t > 1.0) fog_t = 1.0;
        }
        /* Far/medium FLAT leaves: one quad = the whole preview; the close FEA0
         * flat (a0=skybox+0x10) is the dome's authored BACKSTOP. All on the
         * opening plane: in-group painter order at equal depth lets the dome
         * (drawn last) win per pixel. */
        int is_skybox_a0 = (a0 >= 0x80010000u && a0 < 0x80200000u);
        /* --no-lod always-dome: the .s renders far portals as a FLAT colour and
         * only projects the geometric destination-sky dome when close (the
         * distance crossfade). Under --no-lod render the dome at ANY distance:
         * skybox from the owning portal (p+0x00), the shared dome rotation
         * derived natively (derive_dome_matrix - RenderPortals computes it on
         * its own stack, unreachable at leaf time), fed through the same
         * GTE-ctrl path the sub-tick replay uses. The flat quad stays as the
         * dome's backstop, exactly like the close-range FEA0 backstop - but
         * with the CLOSE-BASE colour [m_Skybox+0x10], NOT the leaf's far fog
         * colour: the dome covers only the sky ABOVE its horizon, and a
         * distant portal's opening can sit entirely BELOW it, where the far fog
         * colour (night ≈ black) would leave the opening empty. The .s's own
         * close look fills below-horizon with [skybox+0x10]. */
        int forced_dome = 0; uint32_t forced_sb = 0; int forced_M[3][3];
        uint32_t forced_fp = 0;
        uint32_t flat_rgb_override = PORTAL_RGB_RESOLVE;
        /* Close-backstop test (exact): the close FEA0 flat carries a0 =
         * m_Skybox+0x10 of some portal; far/medium flats carry a stack/global
         * colour ptr (which can land inside the loose is_skybox_a0 RANGE -
         * the PSX stack lives at 0x801FFxxx - so match the portal list). */
        const uint32_t skybox_owner = portal_by_skybox_link(cpu, a0);
        const int a0_is_close_backstop = (skybox_owner != 0);
        int nolod_force = (nolod_portal_dome_on() && kind == PORTAL_FLAT && !a0_is_close_backstop);
        if (nolod_force) {
            int ry0=(int32_t)cpu->read_word(D_CLIPRECT+0x0)>>16, ry1=(int32_t)cpu->read_word(D_CLIPRECT+0x4)>>16;
            int rx0=(int32_t)cpu->read_word(D_CLIPRECT+0x8)>>16, rx1=(int32_t)cpu->read_word(D_CLIPRECT+0xC)>>16;
            uint32_t fp = owning_portal_ptr(cpu, &s_view, a0, (rx0+rx1)*0.5, (ry0+ry1)*0.5);
            uint32_t sb = fp ? cpu->read_word(fp + 0x00) : 0;
            if (sb >= 0x80010000u && sb < 0x80200000u) {
                forced_fp = fp;
                forced_sb = sb;   /* provisional; cleared if the dome feed fails below */
                uint32_t cb = portal_flat_raw_rgb(cpu, sb + 0x10, 0);
                if (cb) flat_rgb_override = cb;
                /* The dome and backstop stand in for the far flat, so they
                 * take its fog term toward [g_Cyclorama+0x10]. */
                fog_t = portal_far_fog_t(cpu, &s_view, fp, portal_forced_dome_fog_mode(), &fog);
            }
        }
        /* The portal preview is a far BACKGROUND seen through the arch window: it
         * must be occluded by the arch stone but must NEVER occlude actors/Spyro
         * (who can be at/behind the portal plane while walking through). Stamp it
         * PORTAL_CYCLORAMA (passed to add_tri inside feed_portal_quad/_dome) so the
         * renderer draws it depth-test-on / depth-WRITE-off in its own pass before
         * the actors. */
        feed_portal_quad(cpu, a0, is_dome, forced_fp,
                         flat_rgb_override, 0, fog, fog_t);
        if (forced_sb) {
            uint32_t sb = forced_sb; forced_sb = 0;
            { static int fd=-1; if(fd<0){const char*e=getenv("OPENPETE_NOLOD_DOME_DBG");fd=(e&&*e&&*e!='0');}
              if (fd) fprintf(stderr, "[nolod-dome f=%u] a0=%08X fp=%08X sb=%08X d=%d far_fog=%06X t=%.4f\n",
                              openpete_frame, a0, forced_fp, sb,
                              (int)portal_game_distance(cpu, s_view.cam_ram, forced_fp), fog, fog_t); }
            {
                derive_dome_matrix(cpu, forced_M);
                uint32_t save_gte[5];
                for (int i = 0; i < 5; i++) save_gte[i] = cpu->gte_ctrl[i];
                cpu->gte_ctrl[0] = (uint32_t)(uint16_t)forced_M[0][0] | ((uint32_t)(uint16_t)forced_M[0][1] << 16);
                cpu->gte_ctrl[1] = (uint32_t)(uint16_t)forced_M[0][2] | ((uint32_t)(uint16_t)forced_M[1][0] << 16);
                cpu->gte_ctrl[2] = (uint32_t)(uint16_t)forced_M[1][1] | ((uint32_t)(uint16_t)forced_M[1][2] << 16);
                cpu->gte_ctrl[3] = (uint32_t)(uint16_t)forced_M[2][0] | ((uint32_t)(uint16_t)forced_M[2][1] << 16);
                cpu->gte_ctrl[4] = (uint32_t)(uint16_t)forced_M[2][2];
                int fret = feed_portal_dome(cpu, sb, 0, /*canonical=*/1, fog, fog_t);
                { static int fd=-1; if(fd<0){const char*e=getenv("OPENPETE_NOLOD_DOME_DBG");fd=(e&&*e&&*e!='0');}
                  if (fd) fprintf(stderr, "[nolod-dome f=%u] ret=%d\n", openpete_frame, fret); }
                if (fret) { forced_dome = 1; forced_sb = sb; }
                for (int i = 0; i < 5; i++) cpu->gte_ctrl[i] = save_gte[i];
            }
        }
        if (is_dome && is_skybox_a0)
            feed_portal_dome(cpu, a0, 0, /*canonical=*/1, fog, fog_t);
        /* Record which portal this leaf rendered (the .s only renders on-4:3 portals)
         * so the widescreen off-4:3 reveal below doesn't double-draw it. Also the
         * sub-tick capture (replayed by portal_reextract every present). */
        {
            int ry0=(int32_t)cpu->read_word(D_CLIPRECT+0x0)>>16, ry1=(int32_t)cpu->read_word(D_CLIPRECT+0x4)>>16;
            int rx0=(int32_t)cpu->read_word(D_CLIPRECT+0x8)>>16, rx1=(int32_t)cpu->read_word(D_CLIPRECT+0xC)>>16;
            uint32_t fp = owning_portal_ptr(cpu, &s_view, a0, (rx0+rx1)*0.5, (ry0+ry1)*0.5);
            if (fp && s_fed_n < 8) s_fed_portal[s_fed_n++] = fp;
            if (s_pcap_n < PORTAL_CAP_MAX) {
                portal_cap_t* c = &s_pcap[s_pcap_n++];
                /* A skybox-linked flat (the close base / backstop, a0 = m_Skybox
                 * or +0x10) does not resolve through owning_portal_ptr, which
                 * keys on the leaf's colour pointer. Without an owning portal
                 * the sub-tick replay cannot recompute the clip rect from the
                 * LIVE outline and falls back to the captured one - the stale
                 * corners then disagree with the dome (re-projected through the
                 * lerped matrix) by a pixel or two at the silhouette, and the
                 * flat's single colour paints the gap. Only intermediate
                 * presents show it; the canonical rect is never stale.
                 * Match the portal by its skybox, exactly as the close-backstop
                 * test above does. */
                const uint32_t owner = fp ? fp : skybox_owner;
                c->portal        = owner;
                c->flat_a0       = a0;
                /* raw, pre-fog; the forced --no-lod dome overrides the far fog
                 * colour with the destination close-base (below-horizon fill) */
                c->flat_rgb      = (flat_rgb_override != PORTAL_RGB_RESOLVE)
                                       ? flat_rgb_override
                                       : portal_flat_raw_rgb(cpu, a0, is_dome);
                c->is_dome       = (is_dome && is_skybox_a0) || forced_dome;
                c->kind          = s_portal_kind;
                c->skybox        = (is_dome && is_skybox_a0) ? a0 : forced_sb;
                c->ot_b          = cpu->read_word(D_PORTAL_DEPTH);
                c->fog_rgb       = fog;
                c->fog_t         = fog_t;
                c->far_fog       = (forced_fp != 0);
                c->clip[0] = ry0; c->clip[1] = ry1; c->clip[2] = rx0; c->clip[3] = rx1;
                c->rect_rule     = portal_rect_rule_for(cpu, owner, kind, c->clip);
                c->src           = BUGREC_SUB_PORTAL_LEAF;
                c->frame         = openpete_frame;
                /* Snapshot the shared dome matrix (same for every dome leaf this
                 * frame); the prev copy was rolled in on_80050BE0. */
                c->derived_dome  = forced_dome;
                if (c->is_dome && !forced_dome) { read_gte_rotmatrix(cpu, s_dome_curr); s_dome_curr_valid = 1; }
            }
        }
    }
    s_leaf_in = 0;
    return 1;
}

/** @brief Dispatch hook for func_8004FEA0, the flat-colour leaf (far preview, close backstop).
 *
 * @param cpu The guest CPU state.
 * @param a The hooked address (unused).
 * @return The leaf_hook verdict. */
static int on_8004FEA0(CPUState* cpu, uint32_t a) {
    OP_ROLE_ASSERT(sim); (void)a; return leaf_hook(cpu, "FEA0(close)",     func_8004FEA0, PORTAL_FLAT);    }
/** @brief Dispatch hook for func_80050240, the dome leaf fogged toward D_800757D4.
 *
 * @param cpu The guest CPU state.
 * @param a The hooked address (unused).
 * @return The leaf_hook verdict. */
static int on_80050240(CPUState* cpu, uint32_t a) {
    OP_ROLE_ASSERT(sim); (void)a; return leaf_hook(cpu, "50240(blended)",  func_80050240, PORTAL_BLENDED); }
/** @brief Dispatch hook for func_8004F4BC, the unfogged gouraud dome leaf.
 *
 * @param cpu The guest CPU state.
 * @param a The hooked address (unused).
 * @return The leaf_hook verdict. */
static int on_8004F4BC(CPUState* cpu, uint32_t a) {
    OP_ROLE_ASSERT(sim); (void)a; return leaf_hook(cpu, "F4BC(secondary)", func_8004F4BC, PORTAL_DOME);    }

/** @brief Draw the destination-sky previews of the portals RenderPortals culled but the view shows.
 *
 * The .s RenderPortals culls portals whose centroid is off the 4:3 screen, and
 * distance-culls far ones, so a widened view shows an arch in the side margin
 * with no preview, and --no-lod terrain shows a far arch with an empty opening.
 * After the real RenderPortals has fed its portals through the leaves, this
 * walks g_Portals and, for each portal no leaf fed whose outline has a part
 * in front of the camera (portal_outline_clipped), whose outline bbox
 * reaches the widened view, and (at
 * stock LOD) is not deep inside the 4:3 frame, draws the flat outline fill
 * through feed_portal_quad, plus the gradient dome when the outline's mean
 * view-Z is below the .s's dome range 16384, fading toward the flat colour over
 * [12288, 16384), or always under the --no-lod dome, which takes the far
 * flat's fog term (portal_far_fog_t) instead. Each reveal is captured
 * for the sub-tick replay. Returns at once on a canonical 4:3 view without
 * --no-lod. The GTE rotation registers are saved and restored. D_CLIPRECT is
 * staged in the sub-tick view when one is open, else written and restored to
 * its saved value before return.
 * @param cpu guest CPU state */
static void feed_revealed_portals(CPUState* cpu) OP_REQUIRES(sim) {
    ngte_view_build(cpu, &s_view);
    int nolod = nolod_portal_dome_on();
    /* Widescreen reveals off-4:3 portals; --no-lod ALSO reveals
     * distance-culled ones at any aspect - the no-lod terrain renders the
     * arch at any range, but the .s RenderPortals distance-culls the portal
     * itself, which would leave a visible arch with an empty opening. */
    if (!s_view.extended && !nolod) return;   /* canonical view: the .s draws every portal it admits */
    const float xlo = s_view.xlo, xhi = s_view.xhi;
    uint32_t n = cpu->read_word(G_PORTALCOUNT); if (n > 6) n = 6;
    int revealed = 0;
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);   /* append; world/cyclorama began it */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = cpu->read_word(G_PORTALS + i * 4);
        if (p < 0x80010000u || p >= 0x80200000u) continue;
        int fed = 0;                                     /* skip a portal a leaf already drew (on-4:3) */
        for (int k = 0; k < s_fed_n; k++) if (s_fed_portal[k] == p) { fed = 1; break; }
        if (fed) continue;
        uint32_t skybox = cpu->read_word(p + 0x00);
        if (skybox < 0x80010000u || skybox >= 0x80200000u) continue;
        ppt poly[PORTAL_POLY_MAX];
        int clipped = 0;
        int pn = portal_outline_clipped(cpu, &s_view, p, 1, poly, PORTAL_POLY_MAX, &clipped);
        if (pn < 3) continue;                            /* wholly behind the camera */
        float oxlo = poly[0].x, oxhi = poly[0].x, oylo = poly[0].y, oyhi = poly[0].y;
        for (int k = 1; k < pn; k++) {
            if (poly[k].x < oxlo) oxlo = poly[k].x;
            if (poly[k].x > oxhi) oxhi = poly[k].x;
            if (poly[k].y < oylo) oylo = poly[k].y;
            if (poly[k].y > oyhi) oyhi = poly[k].y;
        }
        /* Gate by the outline BBOX, not its centroid: a centroid test skips
         * an opening that straddles a boundary while its centroid lies
         * outside the widened bounds or inside the 4:3 frame.
         *   - a portal fully INSIDE the 4:3 frame (small margin) is
         *     canonical - the .s renders it (or wouldn't at 4:3 either);
         *   - a portal whose bbox misses the widened view entirely is
         *     invisible;
         *   - everything else (straddling either boundary, un-fed) gets the
         *     reveal. A leaf-fed portal was already skipped above, so the .s
         *     keeps authority wherever it actually drew. */
        /* Deep on-4:3 = "the .s renders it" holds only at STOCK LOD - under
         * --no-lod the .s still distance-culls centred portals whose arch the
         * no-lod terrain shows, so the fed check alone decides there. */
        if (!nolod && oxlo >= 8.0f && oxhi <= 504.0f) continue;  /* deep on-4:3 - canonical */
        if (oxhi < xlo || oxlo > xhi) continue;           /* fully outside the widened view */
        uint32_t rgb = cpu->read_word(skybox + 0x18) & 0xFFFFFF;   /* destination sky bg colour */
        /* Destination DOME for revealed portals, distance rule mirrored from
         * RenderPortals.
         * The .s leaf selection (0x80051858) draws the dome whenever its
         * distance metric t4 < 16384, with the medium band [12288,16384)
         * DPCS-fading the dome toward the flat colour by (t4-12288)/4096 - at
         * the boundary the dome is 100% faded, so the far flat-only side
         * crossfades seamlessly. t4 tracks the OUTLINE MEAN view-Z at
         * t4/mean ~ 0.956 (about 2% mirror error). The reveal fades
         * toward its OWN flat colour (the destination bg it fans below) so the
         * crossfade converges on what the far reveal already shows. --no-lod
         * keeps its unconditional un-faded dome. */
        double rev_mean = 0.0;
        for (int k = 0; k < pn; k++) rev_mean += poly[k].vz;
        rev_mean /= pn;
        int rev_nolod = nolod_portal_dome_on();
        int rev_want = rev_nolod || rev_mean < 16384.0;
        uint32_t rev_fog = 0; double rev_fog_t = 0.0;
        if (rev_want && !rev_nolod && rev_mean > 12288.0) {
            rev_fog = rgb;
            rev_fog_t = (rev_mean - 12288.0) / 4096.0;
            if (rev_fog_t > 1.0) rev_fog_t = 1.0;
        }
        /* The forced dome of a culled portal takes the far flat's fog term,
         * as the leaf-fed forced dome does. */
        if (rev_nolod)
            rev_fog_t = portal_far_fog_t(cpu, &s_view, p, portal_forced_dome_fog_mode(), &rev_fog);
        /* The base clip rect (RenderPortals set none for a culled portal) is
         * the projected-outline bbox, published through the sub-tick view when
         * one is open (the feeders read it back through cpu->read_word), else
         * written into D_CLIPRECT and restored. The flat preview goes through
         * feed_portal_quad, the mirror of the flat leaf func_8004FEA0 that the
         * game would have drawn for this portal on screen and the emitter the
         * sub-tick replay runs on this capture, so both paths emit one
         * primitive set. */
        uint32_t save_clip[4] = {0};
        int rev_rc[4];
        portal_outline_rect(&s_view, poly, pn, clipped, 1, rev_rc);
        const uint32_t rev_clip[4] = {
            (uint32_t)rev_rc[0] << 16,
            (uint32_t)rev_rc[1] << 16,
            (uint32_t)rev_rc[2] << 16,
            (uint32_t)rev_rc[3] << 16,
        };
        uint8_t* rev_clip_st =
            (uint8_t*)openpete_subtick_view_stage(D_CLIPRECT, sizeof rev_clip);
        if (rev_clip_st) {
            memcpy(rev_clip_st, rev_clip, sizeof rev_clip);
        } else {
            for (int j = 0; j < 4; j++) save_clip[j] = cpu->read_word(D_CLIPRECT + j * 4u);
            for (int j = 0; j < 4; j++) cpu->write_word(D_CLIPRECT + j * 4u, rev_clip[j]);
        }
        s_portal_kind = rev_want ? OPENPETE_TAG_PORTAL_DOME : OPENPETE_TAG_PORTAL_FLAT;
        feed_portal_quad(cpu, 0, 0, p, rgb, 0, rev_fog, rev_fog_t);
        revealed++;
        /* Same recipe as the leaf-hook forced dome: derived shared matrix via
         * gte_ctrl. canonical=0 + hint 0: a culled portal's live
         * D_PORTAL_DEPTH is another portal's bucket. */
        int rev_dome = 0; int rev_M[3][3];
        if (rev_want) {
            derive_dome_matrix(cpu, rev_M);
            uint32_t save_gte[5];
            for (int j = 0; j < 5; j++) save_gte[j] = cpu->gte_ctrl[j];
            cpu->gte_ctrl[0] = (uint32_t)(uint16_t)rev_M[0][0] | ((uint32_t)(uint16_t)rev_M[0][1] << 16);
            cpu->gte_ctrl[1] = (uint32_t)(uint16_t)rev_M[0][2] | ((uint32_t)(uint16_t)rev_M[1][0] << 16);
            cpu->gte_ctrl[2] = (uint32_t)(uint16_t)rev_M[1][1] | ((uint32_t)(uint16_t)rev_M[1][2] << 16);
            cpu->gte_ctrl[3] = (uint32_t)(uint16_t)rev_M[2][0] | ((uint32_t)(uint16_t)rev_M[2][1] << 16);
            cpu->gte_ctrl[4] = (uint32_t)(uint16_t)rev_M[2][2];
            rev_dome = feed_portal_dome(cpu, skybox, 0, /*canonical=*/0, rev_fog, rev_fog_t);
            for (int j = 0; j < 5; j++) cpu->gte_ctrl[j] = save_gte[j];
        }
        if (!rev_clip_st)
            for (int j = 0; j < 4; j++) cpu->write_word(D_CLIPRECT + j * 4u, save_clip[j]);
        if (s_pcap_n < PORTAL_CAP_MAX) {
            portal_cap_t* c = &s_pcap[s_pcap_n++];
            c->portal        = p;
            c->flat_a0       = 0;
            c->flat_rgb      = rgb;
            c->is_dome       = rev_dome;
            c->kind          = s_portal_kind;
            c->rect_rule     = PORTAL_RECT_OUTLINE;
            c->derived_dome  = rev_dome;
            c->skybox        = rev_dome ? skybox : 0;
            c->ot_b          = 0;      /* revealed off-4:3: no live leaf -> outline mirror */
            c->fog_rgb       = rev_fog;
            c->fog_t         = rev_fog_t;
            c->far_fog       = rev_nolod;
            c->src           = BUGREC_SUB_PORTAL_REVEAL;
            c->frame         = openpete_frame;
            memcpy(c->clip, rev_rc, sizeof c->clip);
        }
    }
    static int s_logged = 0;
    if (!s_logged && revealed > 0) {
        s_logged = 1;
        fprintf(stderr, "[portal-reveal] frame=%u F=%.3f s=%.3f drew %d off-4:3 portal preview(s) "
                "the .s culled\n", openpete_frame, s_view.F, s_view.s, revealed);
    }
}

static int s_rp_in = 0;
static unsigned s_rp_frame = 0xFFFFFFFFu;   /* openpete_frame of the last RenderPortals */

/* Return address of func_8002B9CC's call to the world draw func_800258F0
 * (jal at 0x8002BA50). Every caller of func_8002B9CC runs RenderPortals
 * directly before or directly after it in the same draw (draw.c:
 * func_8001A40C, func_8001CA38 before; func_8001CFDC, func_8001D718,
 * GamestateDraw after); the direct func_800258F0 callers (title screen,
 * credits, func_8001E9C8) draw no portals. */
#define ENV_DRAW_RA 0x8002BA58u

int openpete_portal_capture_follows(CPUState* cpu) {
    return cpu->ra == ENV_DRAW_RA && s_rp_frame != openpete_frame;
}

/** @brief RenderPortals hook.
 *
 * Inert (returns 0) unless the portal dump, the native scene, or a render-path
 * selection is on, and on re-entry. Resets the tick's fed-portal set and
 * capture set, rolls the shared dome matrix to prev, and runs the game's
 * draw, whose leaves feed and capture the on-4:3 portals. With the native
 * scene on it reveals the culled portals, stamps the capture with the
 * canonical-render epoch and marks RP_PORTAL drawn. After the draw it rolls
 * the portal spin angles and g_Camera's rotation angles to prev and records
 * the live D_PORTAL_YAW, D_PORTAL_PITCH and g_Camera +0x4C..+0x51 as curr,
 * then releases a terrain feed that waited for this
 * capture.
 * @param cpu guest CPU state
 * @param a   the hooked address (unused)
 * @return 1 after running the draw, 0 when inert or re-entered */
static int on_80050BE0(CPUState* cpu, uint32_t a) {
    OP_ROLE_ASSERT(sim);
    (void)a;
    int feed = openpete_native_scene_enabled();
    if (!portal_dump_enabled() && !feed && !render_paths_active()) return 0;
    if (s_rp_in) return 0;
    s_rp_in = 1;
    s_fed_n = 0;                              /* reset the per-frame on-4:3 fed-portal dedup set */
    cut_held_sets_capture(&s_sets);           /* the epoch's first run retires the last set */
    s_pcap_n = 0;                             /* reset the per-frame sub-tick capture set */
    /* Roll the shared dome matrix curr->prev for this canonical tick; the leaves
     * below refill curr. prev is valid iff the previous canonical frame drew a dome
     * (so prev/curr are consecutive ticks -> a valid one-tick lerp). */
    memcpy(s_dome_prev, s_dome_curr, sizeof s_dome_prev);
    s_dome_prev_valid = s_dome_curr_valid;
    s_dome_curr_valid = 0;
    static int s_reports = 0;
    if (s_want_dump_this_frame() && s_reports < 8) { ngte_view_build(cpu, &s_view); dump_portal_table(cpu); s_reports++; }
    { OP_PROF_FLAGS_ACCUM_SCOPE("portal.srend", OP_PROF_F_GUEST); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_GUEST);
      func_80050BE0(cpu); }                    /* real RenderPortals: on-4:3 portals fed via leaves */
    if (feed) feed_revealed_portals(cpu);     /* widescreen: off-4:3 portals the .s culled */
    /* The leaves + reveal have populated s_pcap[] this canonical frame; mark it
     * usable so portal_reextract replays it on the intermediate presents.
     * (s_pcap_n may be 0 in a portal-less level - the reextract then no-ops.)
     * The staleness gate is render_path_mark_drawn (liveness unification):
     * the interval-less mark, NOT a polybuf interval - under widescreen the
     * native feed can contain revealed portals the .s culled (empty polybuf
     * emission, real native content), so "RenderPortals ran + fed" is the
     * signal, not "emitted prims". Latched into the canon-drawn mask once per
     * canonical present by the shared render_paths commit. */
    if (feed) { s_clip_presents++;
                memcpy(s_clip_last, s_clip_cur, sizeof s_clip_last);
                memset(s_clip_cur, 0, sizeof s_clip_cur);
                s_pcap_valid = 1; s_pcap_frame = openpete_frame;
                s_pcap_epoch = spyro_native_canon_epoch_stamp();
                render_path_mark_drawn(RP_PORTAL); }
    s_rp_frame = openpete_frame;
    memcpy(s_spin_prev, s_spin_curr, sizeof s_spin_prev);
    s_spin_prev_valid = s_spin_curr_valid;
    s_spin_curr[0] = (double)(int32_t)cpu->read_word(D_PORTAL_YAW);
    s_spin_curr[1] = (double)(int32_t)cpu->read_word(D_PORTAL_PITCH);
    memcpy(s_camang_prev, s_camang_curr, sizeof s_camang_prev);
    s_camang_curr[0] = (double)(int16_t)(cpu->read_word(G_CAMERA + 0x4Cu) & 0xFFFFu);
    s_camang_curr[1] = (double)(int16_t)(cpu->read_word(G_CAMERA + 0x4Cu) >> 16);
    s_camang_curr[2] = (double)(int16_t)(cpu->read_word(G_CAMERA + 0x50u) & 0xFFFFu);
    s_spin_curr_valid = 1;
    { OP_PROF_ACCUM_SCOPE("portal.defer.terrain");  /* timed as terrain_native's */
      terrain_native_flush_deferred(); }      /* a world draw that ran first this tick */
    s_rp_in = 0;
    return 1;
}

/** @brief Module option hook; the portal feed reads no option, so it does nothing.
 *
 * @param opts The parsed options (unused). */
static void portal_native_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
}

/** @brief Load `M` into the GTE rotation registers. */
static void portal_gte_load_rot(CPUState* cpu, const int M[3][3]) {
    cpu->gte_ctrl[0] = (uint32_t)(uint16_t)M[0][0] | ((uint32_t)(uint16_t)M[0][1] << 16);
    cpu->gte_ctrl[1] = (uint32_t)(uint16_t)M[0][2] | ((uint32_t)(uint16_t)M[1][0] << 16);
    cpu->gte_ctrl[2] = (uint32_t)(uint16_t)M[1][1] | ((uint32_t)(uint16_t)M[1][2] << 16);
    cpu->gte_ctrl[3] = (uint32_t)(uint16_t)M[2][0] | ((uint32_t)(uint16_t)M[2][1] << 16);
    cpu->gte_ctrl[4] = (uint32_t)(uint16_t)M[2][2];
}

/** @brief The derived dome matrix of a sub-tick present.
 *
 * derive_dome_matrix with g_Camera's rotation angles and the portal spin
 * angles each lerped between the last two canonical ticks (along the shorter
 * arc of the 4096-unit circle). The camera bracket stages tick N's euler
 * angles beside the lerped camera matrices, so reading them from the staged
 * camera would hold the dome at tick N's rotation while the scene moves. The
 * current tick's angles, read live, when `interp_on` is 0 or no pair exists.
 * At alpha 1 it is the matrix the canonical feed derived. */
static void portal_replay_derived_matrix(CPUState* cpu, double alpha, int interp_on, int M[3][3]) {
    double yaw = s_spin_curr[0], pitch = s_spin_curr[1];
    if (!(interp_on && s_spin_prev_valid)) {
        derive_dome_matrix_at(cpu, yaw, pitch, M);
        return;
    }
    double dy = s_spin_curr[0] - s_spin_prev[0];
    if (dy > 2048.0) dy -= 4096.0; else if (dy < -2048.0) dy += 4096.0;
    yaw   = s_spin_curr[0] - dy * (1.0 - alpha);
    pitch = s_spin_prev[1] + (s_spin_curr[1] - s_spin_prev[1]) * alpha;
    double cam[3];
    for (int k = 0; k < 3; k++) {
        double d = s_camang_curr[k] - s_camang_prev[k];
        if (d > 2048.0) d -= 4096.0; else if (d < -2048.0) d += 4096.0;
        cam[k] = s_camang_curr[k] - d * (1.0 - alpha);
    }
    derive_dome_matrix_cam(cam, yaw, pitch, M);
}

/** @brief Whether a clip rect in either x order is empty: the feeders draw nothing for it.
 * @param rc The rect: min y, max y, then the x pair in either order.
 * @return 1 when empty, else 0. */
static int portal_rect_empty(const int rc[4]) {
    const int xl = rc[2] < rc[3] ? rc[2] : rc[3], xh = rc[2] < rc[3] ? rc[3] : rc[2];
    return xh <= xl || rc[1] <= rc[0];
}

/** @brief The outline rule's rect (feed_revealed_portals' rect) of a portal under the present's view, when its outline reaches the view.
 *
 * The outline is near-clipped (portal_outline_clipped, corrected) and its
 * bbox stored as portal_outline_rect stores it. Refused, with `rc`
 * untouched, when no part of the outline lies in front of the camera or its
 * bbox misses the widened view, the cases the reveal skips.
 * @param cpu The guest CPU state.
 * @param p The Portal* guest address.
 * @param rc Receives the rect: min y, max y, min x, max x.
 * @return 1 with `rc` written, else 0. */
static int portal_view_outline_rect(CPUState* cpu, uint32_t p, int rc[4]) {
    ppt rp[PORTAL_POLY_MAX];
    int clipped = 0;
    const int rn = portal_outline_clipped(cpu, &s_view, p, 1, rp, PORTAL_POLY_MAX, &clipped);
    if (rn < 3) return 0;
    int r[4];
    portal_outline_rect(&s_view, rp, rn, clipped, 1, r);
    if ((float)r[3] < s_view.xlo || (float)r[2] > s_view.xhi || r[1] < 0 || r[0] > PORTAL_SCREEN_H) return 0;
    for (int k = 0; k < 4; k++) rc[k] = r[k];
    return 1;
}

/** @brief A capture's base clip rect under the present's view (s_view), by the capture's rect rule.
 *
 * `rc` holds the captured rect on entry and keeps it when the rule cannot be
 * evaluated. A leaf capture (FLAT, DOME) takes RenderPortals' rect mirror,
 * which clamps to the 4:3 frame and seeds its max x at 0 and, for a dome, its
 * min x at 512: an outline wholly in a Hor+ side strip under this present's
 * camera, such as on the tick the leaf first takes a portal from the reveal,
 * gives an empty rect, on which the feeders draw nothing. In an extended view
 * such a rect is replaced by the outline rule's rect, the one the reveal
 * draws the same outline with; a non-empty game rect and every rect of a
 * canonical (4:3) view are kept. */
static void portal_replay_rect(CPUState* cpu, const portal_cap_t* c, int rc[4]) {
    if (!c->portal) return;
    if (c->rect_rule == PORTAL_RECT_FLAT || c->rect_rule == PORTAL_RECT_DOME) {
        int m[4];
        if (portal_game_rect(cpu, &s_view, c->portal, c->rect_rule == PORTAL_RECT_DOME, m))
            for (int k = 0; k < 4; k++) rc[k] = m[k];
        if (s_view.extended && portal_rect_empty(rc)) portal_view_outline_rect(cpu, c->portal, rc);
        return;
    }
    if (c->rect_rule != PORTAL_RECT_OUTLINE) return;
    ppt rp[PORTAL_POLY_MAX];
    int clipped = 0;
    const int rn = portal_outline_clipped(cpu, &s_view, c->portal, 1, rp, PORTAL_POLY_MAX, &clipped);
    if (rn < 3) return;
    portal_outline_rect(&s_view, rp, rn, clipped, 1, rc);
}

/** @brief The fog a capture's replay applies under the present's view (s_view).
 *
 * A capture holding RenderPortals' far-leaf term re-evaluates it for its
 * portal under the present's camera; any other capture keeps its captured
 * fog. OPENPETE_REPLAY_DBG prints each re-evaluation.
 * @param cpu The guest CPU state.
 * @param c The capture.
 * @param fog_rgb Receives the fog colour, 0x00BBGGRR.
 * @return The blend fraction toward *fog_rgb. */
static double portal_cap_fog(CPUState* cpu, const portal_cap_t* c, uint32_t* fog_rgb) {
    *fog_rgb = c->fog_rgb;
    if (!c->far_fog) return c->fog_t;
    const double t = portal_far_fog_t(cpu, &s_view, c->portal, portal_forced_dome_fog_mode(), fog_rgb);
    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_REPLAY_DBG"); dbg = (e && *e && *e != '0'); }
    if (dbg)
        fprintf(stderr, "[replay-far-fog f=%u] portal=%08X cam=(%.1f,%.1f,%.1f) interp=%d t=%.4f cap_t=%.4f fog=%06X\n",
                openpete_frame, c->portal, s_view.cam[0], s_view.cam[1], s_view.cam[2], s_view.cam_interp,
                t, c->fog_t, *fog_rgb);
    return t;
}

/** @brief The shared dome matrix of a present, from the two ticks' snapshots.
 *
 * Outside a hold, lerp(prev, curr, alpha) componentwise, like the spine's
 * camera matrix: the camera rotation and the portal's spin are baked into both
 * snapshots, so both interpolate without extrapolation. It snaps to curr when
 * the camera is not bracketed (paused or loading), the prev snapshot is
 * missing, or a component moved more than 1200 in the tick, a discontinuity
 * the spine did not catch (a continuous dome moves a few hundred Q12 units a
 * tick). On a hold the snapshot of the shown set's RenderPortals run is the
 * matrix, whatever the cut did to the other one.
 * @param alpha The sub-tick fraction.
 * @param interp_on 1 when the camera is bracketed.
 * @param M Receives the matrix when it is ready.
 * @return 1 when the snapshot the present reads is valid, else 0. */
static int portal_replay_dome_matrix(double alpha, int interp_on, int M[3][3]) {
    const int held_prev = s_pcap_pick == CUT_HELD_PREV;
    if (held_prev ? !s_dome_prev_valid : !s_dome_curr_valid) return 0;
    if (held_prev) {
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) M[i][j] = s_dome_prev[i][j];
        return 1;
    }
    int do_lerp = interp_on && s_dome_prev_valid && s_pcap_pick < 0;
    for (int i = 0; do_lerp && i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (labs((long)s_dome_curr[i][j] - s_dome_prev[i][j]) > 1200) do_lerp = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            if (do_lerp) {
                double v = (double)s_dome_prev[i][j] +
                           ((double)s_dome_curr[i][j] - s_dome_prev[i][j]) * alpha;
                M[i][j] = (int)(v >= 0.0 ? v + 0.5 : v - 0.5);
            } else {
                M[i][j] = s_dome_curr[i][j];
            }
        }
    return 1;
}

/** @brief Whether OPENPETE_PORTAL_PRESENT_DBG prints one [portal-present] line per replayed portal and per refused replay, read once.
 * @return Nonzero when on. */
static int portal_present_dbg(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_PORTAL_PRESENT_DBG"); on = (e && *e && *e != '0'); }
    return on;
}

/* OPENPETE_PORTAL_PRESENT_CENSUS=<first key>,<last key>: per re-extract in
 * that feed-key range, the present and every portal it replayed, held in
 * memory and printed at exit. One present record per portal_reextract call,
 * its portals following it in s_pcen_port. */
#define PCEN_PRESENTS (1u << 16)
#define PCEN_PORTALS  (1u << 18)
typedef struct {
    unsigned key, vbl;       /* feed key and VBL at production */
    float    alpha;          /* the present's sub-tick alpha */
    uint8_t  canon;          /* 1 on the canonical leg (g_openpete_canon_lag_leg) */
    uint8_t  status;         /* 0 replayed, else BUGREC_SUB_PORTAL_NONE_EMPTY or _NONE_GATE */
    int8_t   pick;           /* the held set shown, -1 outside a hold */
    uint32_t tick_curr;      /* openpete_frame of the interp ring's newest snapshot, 0 unpaired */
    uint32_t rp_frame;       /* openpete_frame of the newest RenderPortals run at production */
    uint32_t first, n;       /* its portal records in s_pcen_port */
} pcen_present_t;
typedef struct {
    uint32_t portal, cap_frame;
    uint16_t tris, mask_tris;
    uint8_t  src, dome;
    int16_t  x0, x1;
} pcen_portal_t;
static pcen_present_t* s_pcen_pres;
static pcen_portal_t*  s_pcen_port;
static unsigned        s_pcen_np, s_pcen_nt;   /* records held */
static unsigned long   s_pcen_lost;            /* records past either buffer's end */
static int             s_pcen_k0 = -2, s_pcen_k1;
static int             s_pcen_open;            /* the current re-extract is recorded */

/** @brief Whether a portal is drawn (colour triangles emitted) on a census present.
 * @param pr The present.
 * @param portal The Portal* address.
 * @return 1 when drawn, else 0. */
static int pcen_drawn(const pcen_present_t* pr, uint32_t portal) {
    for (uint32_t k = 0; k < pr->n; k++)
        if (s_pcen_port[pr->first + k].portal == portal && s_pcen_port[pr->first + k].tris > 0) return 1;
    return 0;
}

/** @brief A census present's production order against RenderPortals: tick_curr less rp_frame.
 *
 * RenderPortals runs in the tick's draw, one VBL before the interp ring
 * stamps that tick's snapshot, so the value is 1 when the replayed set is the
 * newest snapshot's tick's, above 1 when the present was produced before that
 * tick's RenderPortals ran, and below 1 when the next tick's RenderPortals ran
 * before the present was produced (a set one tick newer than the span it
 * interpolates). An unpaired present reads 1.
 * @param pr The present.
 * @return The difference. */
static long pcen_order(const pcen_present_t* pr) {
    return pr->tick_curr ? (long)pr->tick_curr - (long)pr->rp_frame : 1;
}

/** @brief The census name of a present's replay status.
 * @param status 0, BUGREC_SUB_PORTAL_NONE_EMPTY or BUGREC_SUB_PORTAL_NONE_GATE.
 * @return "replayed", "empty" or "gate". */
static const char* pcen_status_name(int status) {
    if (status == BUGREC_SUB_PORTAL_NONE_EMPTY) return "empty";
    return status == BUGREC_SUB_PORTAL_NONE_GATE ? "gate" : "replayed";
}

/** @brief Print one census present's line, then one line per portal it replayed.
 * @param i The present's index. */
static void pcen_print_present(unsigned i) {
    const pcen_present_t* pr = &s_pcen_pres[i];
    const long d = pcen_order(pr);
    fprintf(stderr, "[portal-census p] i=%u key=%u vbl=%u leg=%s alpha=%.4f tick_curr=%u rp_frame=%u d=%ld%s "
            "pick=%d status=%s n=%u\n", i, pr->key, pr->vbl, pr->canon ? "canon" : "inter", pr->alpha,
            pr->tick_curr, pr->rp_frame, d, d > 1 ? " before" : d < 1 ? " ahead" : "", pr->pick,
            pcen_status_name(pr->status), pr->n);
    for (uint32_t k = 0; k < pr->n; k++) {
        const pcen_portal_t* e = &s_pcen_port[pr->first + k];
        fprintf(stderr, "[portal-census t] i=%u portal=%08X src=%s dome=%u cap_frame=%u tris=%u mask=%u x=[%d,%d]\n",
                i, e->portal, e->src == BUGREC_SUB_PORTAL_REVEAL ? "reveal" : "leaf", e->dome, e->cap_frame,
                e->tris, e->mask_tris, e->x0, e->x1);
    }
}

/** @brief Collect the distinct portals the census recorded, in first-seen order.
 * @param ids Receives at most `max` Portal* addresses.
 * @param max The capacity of `ids`.
 * @return The count written. */
static int pcen_portal_ids(uint32_t* ids, int max) {
    int n = 0;
    for (unsigned t = 0; t < s_pcen_nt; t++) {
        int j = 0;
        while (j < n && ids[j] != s_pcen_port[t].portal) j++;
        if (j == n && n < max) ids[n++] = s_pcen_port[t].portal;
    }
    return n;
}

/** @brief Count, and print, one portal's FLICKs: drawn on a present, absent on 1 to 32 later presents, drawn again.
 * @param portal The Portal* address.
 * @return The FLICKs found. */
static unsigned pcen_flicks(uint32_t portal) {
    unsigned n = 0;
    long last = -1;
    for (unsigned i = 0; i < s_pcen_np; i++) {
        if (!pcen_drawn(&s_pcen_pres[i], portal)) continue;
        const long gap = last < 0 ? 0 : (long)i - last - 1;
        if (gap >= 1 && gap <= 32) {
            n++;
            fprintf(stderr, "[portal-census flick] portal=%08X drawn i=%ld absent %ld presents drawn i=%u\n",
                    portal, last, gap, i);
        }
        last = (long)i;
    }
    return n;
}

/** @brief Print the census at exit: one line per present and per replayed portal, the FLICKs, then the summary.
 *
 * The summary counts the FLICKs over every portal and the presents produced
 * before or ahead of their tick's RenderPortals run (pcen_order). */
static void pcen_report(void) {
    fprintf(stderr, "[portal-census] presents=%u portals=%u lost=%lu keys=[%d,%d]\n",
            s_pcen_np, s_pcen_nt, s_pcen_lost, s_pcen_k0, s_pcen_k1);
    unsigned before = 0, ahead = 0;
    for (unsigned i = 0; i < s_pcen_np; i++) {
        pcen_print_present(i);
        const long d = pcen_order(&s_pcen_pres[i]);
        before += d > 1; ahead += d < 1;
    }
    uint32_t ids[8];
    const int nid = pcen_portal_ids(ids, 8);
    unsigned flicks = 0;
    for (int j = 0; j < nid; j++) flicks += pcen_flicks(ids[j]);
    fprintf(stderr, "[portal-census summary] presents=%u portals_seen=%d flicks=%u (drawn, absent 1..32 presents, drawn) "
            "before=%u ahead=%u\n", s_pcen_np, nid, flicks, before, ahead);
}

/** @brief Open the census record of one re-extract when OPENPETE_PORTAL_PRESENT_CENSUS names its feed key.
 *
 * Reads the variable once ("<first key>,<last key>"), allocates both buffers
 * then and registers pcen_report at exit.
 * @param alpha The present's sub-tick alpha. */
static void pcen_begin(double alpha) {
    s_pcen_open = 0;
    if (s_pcen_k0 == -2) {
        const char* e = getenv("OPENPETE_PORTAL_PRESENT_CENSUS");
        s_pcen_k0 = -1;
        if (e && sscanf(e, "%d,%d", &s_pcen_k0, &s_pcen_k1) == 2 && s_pcen_k0 >= 0) {
            s_pcen_pres = (pcen_present_t*)calloc(PCEN_PRESENTS, sizeof *s_pcen_pres);
            s_pcen_port = (pcen_portal_t*)calloc(PCEN_PORTALS, sizeof *s_pcen_port);
            if (s_pcen_pres && s_pcen_port) atexit(pcen_report); else s_pcen_k0 = -1;
        } else {
            s_pcen_k0 = -1;
        }
    }
    if (s_pcen_k0 < 0) return;
    const int key = (int)openpete_feed_key_now();
    if (key < s_pcen_k0 || key > s_pcen_k1) return;
    if (s_pcen_np >= PCEN_PRESENTS) { s_pcen_lost++; return; }
    pcen_present_t* pr = &s_pcen_pres[s_pcen_np++];
    memset(pr, 0, sizeof *pr);
    pr->key = (unsigned)key; pr->vbl = openpete_frame; pr->alpha = (float)alpha;
    pr->canon = (uint8_t)(g_openpete_canon_lag_leg != 0);
    pr->pick = (int8_t)s_pcap_pick;
    pr->rp_frame = s_rp_frame;
    pr->first = s_pcen_nt;
    double a = 0.0;
    const openpete_interp_snapshot_t *prev = NULL, *curr = NULL;
    if (openpete_interp_query(openpete_interp_present_now(), g_openpete_tick_period_ns, &a, &prev, &curr) && curr)
        pr->tick_curr = curr->tick;
    s_pcen_open = 1;
}

/** @brief Record one portal a re-extract replayed, or why it replayed none, in the sub-tick ring and on the debug line.
 *
 * @param c The capture replayed, or NULL for a status entry.
 * @param src BUGREC_SUB_PORTAL_NONE_EMPTY or _NONE_GATE for a status entry; ignored with a capture.
 * @param alpha The present's sub-tick alpha.
 * @param rc The replay's base clip rect (min y, max y, x pair), or NULL.
 * @param tris The colour triangles the capture's feed emitted.
 * @param mask_tris The mask triangles it emitted. */
static void portal_present_note(const portal_cap_t* c, int src, double alpha, const int* rc,
                                unsigned tris, unsigned mask_tris) {
    openpete_bugrec_sub_portal_t e;
    memset(&e, 0, sizeof e);
    e.portal    = c ? c->portal : 0u;
    e.cap_frame = c ? c->frame : s_pcap_frame;
    e.cap_epoch = s_sets.epoch[0];
    e.tris      = (uint16_t)(tris > 0xFFFFu ? 0xFFFFu : tris);
    e.mask_tris = (uint16_t)(mask_tris > 0xFFFFu ? 0xFFFFu : mask_tris);
    e.src       = (uint8_t)(c ? c->src : src);
    e.dome      = (uint8_t)(c && c->is_dome);
    e.pick      = (int8_t)s_pcap_pick;
    if (rc) {
        e.x0 = (int16_t)(rc[2] < rc[3] ? rc[2] : rc[3]);
        e.x1 = (int16_t)(rc[2] < rc[3] ? rc[3] : rc[2]);
    }
    openpete_bugrec_subtick_portal(&e);
    if (s_pcen_open) {
        pcen_present_t* pr = &s_pcen_pres[s_pcen_np - 1];
        if (!c) pr->status = (uint8_t)src;
        else if (s_pcen_nt >= PCEN_PORTALS) s_pcen_lost++;
        else {
            s_pcen_port[s_pcen_nt++] = (pcen_portal_t){ e.portal, e.cap_frame, e.tris, e.mask_tris,
                                                         e.src, e.dome, e.x0, e.x1 };
            pr->n++;
        }
    }
    if (portal_present_dbg())
        fprintf(stderr, "[portal-present] vbl=%u alpha=%.4f leg=%s portal=%08X src=%u cap_frame=%u "
                "cap_epoch=%u pick=%d dome=%u tris=%u mask=%u x=[%d,%d]\n",
                openpete_frame, alpha, g_openpete_canon_lag_leg ? "canon" : "inter", e.portal, e.src,
                e.cap_frame, e.cap_epoch, e.pick, e.dome, tris, mask_tris, e.x0, e.x1);
}

/* ---- sub-tick re-extract; documented in reextract.h ---- */
void portal_reextract(CPUState* cpu, double alpha, int interp_on, int hold) OP_REQUIRES(sim) {
    s_pcap_pick = cut_held_sets_enter(&s_sets, hold);
    /* A hold whose set is the current one has no tick before it to lerp from. */
    if (s_pcap_pick == CUT_HELD_CURR) interp_on = 0;
    pcen_begin(alpha);
    if (s_pcap_n == 0) { portal_present_note(NULL, BUGREC_SUB_PORTAL_NONE_EMPTY, alpha, NULL, 0u, 0u); return; }
    /* Don't replay a STALE capture. During a level-load transition (e.g. warping
     * THROUGH a portal) the normal draw loop stops, so canonical RenderPortals
     * (on_80050BE0) no longer runs and s_pcap freezes at the last homeworld
     * frame (a screen-filling close portal). Replaying it paints a flat 4:3
     * backdrop-colour quad over everything on every intermediate present.
     * (The main cyclorama is still drawn during the transition by a
     * non-RenderPortals path, so render_path_canon_drawn(RP_CYCLORAMA) does not
     * signal that RenderPortals stopped.)
     * RP_PORTAL is marked (render_path_mark_drawn - interval-less, so widescreen-
     * revealed portals with no polybuf emission still count) each canonical present
     * on_80050BE0 runs+feeds, so the bit goes false the moment RenderPortals stops
     * - exactly the staleness signal we need. The gate also holds the native
     * scene on and the path selected. On a held cut the set's own epoch stands
     * in for the canon-drawn term. */
    if (!hook_reextract_ready_held(RP_PORTAL, s_pcap_pick)) {
        portal_present_note(NULL, BUGREC_SUB_PORTAL_NONE_GATE, alpha, NULL, 0u, 0u);
        return;
    }
    ngte_view_build(cpu, &s_view);
    {   /* OPENPETE_LOAD_CENSUS: replay-fire census (liveness-gate verification). */
        static int cen = -1;
        if (cen < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); cen = (e && *e && *e != '0'); }
        static unsigned s_fires = 0; s_fires++;
        static unsigned last = 0xFFFFFFFFu;
        if (cen && openpete_frame / 100u != last) { last = openpete_frame / 100u;
            fprintf(stderr, "[census f=%u PORTAL-REPLAY] fires=%u\n", openpete_frame, s_fires); }
    }
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);   /* append; cyclorama began it */

    int dome_M[3][3];
    const int dome_ready = portal_replay_dome_matrix(alpha, interp_on, dome_M);

    /* Save the transient inputs we overwrite; restore before returning so the
     * game fiber resumes on canonical CPU/RAM state. */
    uint32_t save_clip[4] = {0}, save_gte[5];
    /* Stage the clip rect once for the whole replay, so every per-cap rect
     * below lands in the view instead of guest memory; a refused stage falls
     * back to writing D_CLIPRECT and restoring it after the loop. */
    uint8_t* clip_st = (uint8_t*)openpete_subtick_view_stage(D_CLIPRECT, 4u * 4u);
    if (!clip_st)
        for (int i = 0; i < 4; i++) save_clip[i] = cpu->read_word(D_CLIPRECT + i * 4);
    for (int i = 0; i < 5; i++) save_gte[i]  = cpu->gte_ctrl[i];

    s_clip_replay = 1;
    for (int n = 0; n < s_pcap_n; n++) {
        const portal_cap_t* c = &s_pcap[n];
        /* Base clip rect for the replay: rebuilt under THIS present's camera
         * by the rule the canonical rect came from, never the captured rect
         * itself: the feeders only UNION the base rect with the live outline
         * bbox, so a rect from the canonical camera keeps its stale corners
         * while the arch moves between presents, and under --render-fps a fast
         * pan pushed the corner past the stone silhouette (a sky block outside
         * the arch on intermediate presents only). A leaf capture takes the
         * RenderPortals mirror for its pass (portal_game_rect), which at the
         * captured camera reproduces the game's rect exactly; a revealed
         * portal takes the float outline bbox its canonical feed used. The
         * captured rect is the fallback when no rule applies or the outline
         * lies wholly behind the camera. */
        int rc[4] = { c->clip[0], c->clip[1], c->clip[2], c->clip[3] };
        { static int rd=-1; if(rd<0){const char*e=getenv("OPENPETE_REPLAY_DBG");rd=(e&&*e&&*e!='0');}
          if (rd) fprintf(stderr, "[replay f=%u] cap portal=%08X flat_a0=%08X skybox=%08X is_dome=%d capturedrect=(%d,%d)-(%d,%d) far_fog=%d cap_t=%.4f\n",
                          openpete_frame, c->portal, c->flat_a0, c->skybox, c->is_dome,
                          c->clip[2], c->clip[0], c->clip[3], c->clip[1], c->far_fog, c->fog_t); }
        portal_replay_rect(cpu, c, rc);
        s_portal_kind = c->kind;
        {
            const uint32_t cap_clip[4] = {
                (uint32_t)rc[0] << 16, (uint32_t)rc[1] << 16,
                (uint32_t)rc[2] << 16, (uint32_t)rc[3] << 16,
            };
            if (clip_st) memcpy(clip_st, cap_clip, sizeof cap_clip);
            else for (int j = 0; j < 4; j++)
                     cpu->write_word(D_CLIPRECT + j * 4u, cap_clip[j]);
        }

        if (c->is_dome && c->derived_dome) {
            /* A dome the canonical feed drew with the derived matrix replays
             * the same derivation: the shared snapshot holds the matrix a dome
             * leaf left in the GTE, which rounds differently. */
            int dM[3][3];
            portal_replay_derived_matrix(cpu, alpha, interp_on, dM);
            portal_gte_load_rot(cpu, dM);
        } else if (c->is_dome && dome_ready) {
            portal_gte_load_rot(cpu, dome_M);   /* the sub-tick-lerped shared dome matrix */
        }

        /* Self-check (OPENPETE_PORTAL_REEXTRACT_DBG): per-cap replay trace. For a
         * dome, a valid recomposed matrix is a rotation - each row magnitude ≈ 4096
         * (Q12); a packing/algebra bug shows wild magnitudes. Also reports the
         * captured clip rect + capture-frame so a STALE replay (canonical
         * RenderPortals stopped firing during a load transition while s_pcap kept
         * its last value) is visible as a frozen s_pcap_frame. */
        static int s_dbg = -1;
        if (s_dbg < 0) { const char* e = getenv("OPENPETE_PORTAL_REEXTRACT_DBG"); s_dbg = (e && *e && *e != '0'); }
        if (s_dbg && (openpete_frame % 20u) == 0) {
            double rowmag = 0.0;
            if (c->is_dome) {
                int dm00 = (int16_t)(cpu->gte_ctrl[0] & 0xFFFF), dm01 = (int16_t)(cpu->gte_ctrl[0] >> 16);
                int dm02 = (int16_t)(cpu->gte_ctrl[1] & 0xFFFF);
                rowmag = sqrt((double)dm00*dm00 + (double)dm01*dm01 + (double)dm02*dm02);
            }
            fprintf(stderr, "[portal-reextract f=%u capframe=%u] cap %d/%d %s p=%08X rgb=%06X clip y[%d,%d] x[%d,%d]%s\n",
                    openpete_frame, s_pcap_frame, n, s_pcap_n,
                    c->is_dome ? "DOME" : "flat", c->portal, c->flat_rgb,
                    c->clip[0], c->clip[1], c->clip[2], c->clip[3],
                    c->is_dome ? "" : "");
            if (c->is_dome) fprintf(stderr, "    |dome row0|=%.0f (≈4096?)\n", rowmag);
        }

        /* Same emit the canonical leaf used, driven by the captured decision.
         * PORTAL_CYCLORAMA so the sub-tick previews stay depth-write-off too (never
         * occlude Spyro flying/walking through). PORTAL_CYCLORAMA is passed to add_tri
         * inside feed_portal_quad/_dome. */
        uint32_t fog_rgb = 0;
        const double fog_t = portal_cap_fog(cpu, c, &fog_rgb);
        const unsigned col0 = s_tri_col, mask0 = s_tri_mask;
        feed_portal_quad(cpu, c->flat_a0, 0 /* rgb_override set */, c->portal, c->flat_rgb, c->ot_b,
                         fog_rgb, fog_t);
        if (c->is_dome)
            feed_portal_dome(cpu, c->skybox, c->ot_b, /*canonical=*/0,
                             fog_rgb, fog_t);
        portal_present_note(c, 0, alpha, rc, s_tri_col - col0, s_tri_mask - mask0);
    }

    s_clip_replay = 0;
    if (!clip_st)
        for (int i = 0; i < 4; i++) cpu->write_word(D_CLIPRECT + i * 4, save_clip[i]);
    for (int i = 0; i < 5; i++) cpu->gte_ctrl[i] = save_gte[i];
}

void portal_reextract_leave(void) {
    cut_held_sets_leave(&s_sets, s_pcap_pick);
    s_pcap_pick = -1;
}

static const feature_dispatch_entry_t portal_dispatch[] = {
    { 0x80050BE0u, on_80050BE0, 0u },
    { 0x8004FEA0u, on_8004FEA0, 0u },
    { 0x80050240u, on_80050240, 0u },
    { 0x8004F4BCu, on_8004F4BC, 0u },
    { 0u, NULL, 0u },
};

const feature_module_t spyro1_portal_module = {
    .name           = "portal_native",
    .apply_options  = portal_native_apply_options,
    .dispatch_table = portal_dispatch,
};
