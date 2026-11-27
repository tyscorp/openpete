// games/spyro-1/renderer/transition.c - native feeders for the level
// transition presentation layer: the balloonist / level-exit cross-fade backdrop
// (func_8004F000) and the GS_ExitLevel grayscale freeze (func_8001C694).
//
// func_8004F000 ("cross-fade"): during a level transition or a balloonist
// take-off (GS_Balloonist func_8001E24C, and GS_ExitLevel func_8001C694) the
// game draws the g_Cyclorama dome with every sector colour run through a GTE
// DPCS fade toward D_BLEND_TARGET by D_FOG_FACTOR - the soft dreamy gradient
// backdrop behind the balloon and the "ENTERING <world>" text. It is the same
// dome geometry the main sky renderer (func_8004EBA8) draws, so this feeder
// RE-DERIVES it from gamestate via cyclorama_native_extract_faded rather than
// replaying prims: func_8004F000 culls each face by AND-ing its three vertices'
// outcodes against the 4:3 box [0,512]x[0,256], so the prims it emits stop dead
// at the 4:3 edges and a Hor+ frame was left on background clear beyond them.
//
// The real renderer still runs first (it owns the gameplay verdict and the
// g_SonyImage scratch), and its polybuf interval is still bracketed - that
// range drives the --render-paths OT filter and the RP_TRANSITION liveness
// latch. Only the pixels come from the native extraction.
//
// Display-only / oracle-inert: the extraction only READS g_Cyclorama and
// g_Camera, so no gameplay-readable byte moves.
//
// func_8001C694 (GS_ExitLevel): case 0 is the green-tinted grayscale freeze of
// the level being exited - the SAME effect as the pause/inventory freeze
// backdrop, reproduced from the native renderer's own last world frame via
// openpete_native_scene_set_freeze_backdrop (its FT4s go through the unhooked
// world-OT add func_800168A0, so native never showed them); case 1 is the
// cross-fade, fed by the func_8004F000 hook. The OPENPETE_TRANSITION_PROBE=1 red
// probe is retained as a diagnostic.

#include "feature_registry.h"
#include "psx_runtime_internal.h"            /* CPUState */
#include "openpete_native_render.h" /* native scene API */
#include "renderer/render_paths.h"           /* render_paths_active */
#include "renderer/hook_bracket.h"           /* hook_reextract_ready */
#include "reextract.h" /* transition_reextract decl */
#include "renderer/cut_guard.h" /* cut_held_sets_t: the held cut's tick N-1 set */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro-1/gen/spyro1_symbols.gen.h"
#include "game_layout.h"
#include "openpete_pace_log.h"   /* OP_PACE_SPLIT_SCOPE - the guest call's split */
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE - fiber-hook cost split */
#include "openpete_thread_roles.h"

extern void     func_8004F000(CPUState* cpu);   /* the real cross-fade renderer */
extern void     func_8001C694(CPUState* cpu);   /* the real GS_ExitLevel screen */
extern unsigned openpete_frame;

/* D_800758B8: func_8001C694's case selector - 0 = the green-tinted grayscale
 * freeze of the level being exited; 1 = the colour cross-fade (-> func_8004F000,
 * already fed). (The decomp names it "pause text rotation ticks"; reused here.) */
/* D_8007568C: the per-case fade tick, 0..31 (func_8002E084 increments it each tick,
 * resets at 32 and advances the case). In case 0 it shrinks the grayscale CLUT load
 * (setRECT width 32-tick), fading the frozen level to black. */
/* g_Cyclorama.m_BackgroundColor (g_Cyclorama @0x80078A40 + 0x10): the destination
 * homeworld's flat background colour. In case 1 the game clears the framebuffer to
 * ColorLerp(bg, 0, D_8007575C) = bg*(tick/32) - a linear fade-in from black, drawn
 * BEHIND the cyclorama dome. */

#define CRED   "\033[1;31m"
#define CRESET "\033[0m"

/* ---- the cross-fade dome (screen-space tris) ------------------------------ */
#define MAX_DOME_TRI 8192
static cyclorama_native_tri s_cross[MAX_DOME_TRI];
static int  s_cross_n;

/* The dome each canonical draw extracted: the set a held camera cut shows. The
 * re-extract rewrites s_cross on every present. */
static cyclorama_native_tri  s_tick_buf[2][MAX_DOME_TRI];
static cyclorama_native_tri* s_tick     = s_tick_buf[0];
static cyclorama_native_tri* s_tick_old = s_tick_buf[1];
static int s_tick_n, s_tick_old_n;

/** @brief Exchange the current captured dome with the retired one, with their counts. */
static void cross_sets_swap(void) {
    cyclorama_native_tri* t = s_tick; s_tick = s_tick_old; s_tick_old = t;
    const int n = s_tick_n; s_tick_n = s_tick_old_n; s_tick_old_n = n;
}

static cut_held_sets_t s_sets = { cross_sets_swap, &s_tick_n, "dome", { 0u, 0u }, 0u };
/* Below the dome's horizon (and above its cap, when the camera pitches) the
 * PSX shows the draw-env clear: ColorLerp(g_Cyclorama.m_BackgroundColor,
 * 0x401010, D_8007575C) on the balloonist, ColorLerp(m_BackgroundColor, 0,
 * D_8007575C) on GS_ExitLevel case 1, which writes it into the non-current
 * draw buffer's DRAWENV and PutDrawEnv's that buffer before its DrawOTag.
 * The native scene draws the clear from the env the presented scene was
 * submitted with over the whole target (openpete_native_drawn_env,
 * openpete_native_set_clear_color). */

/* Staleness gate: the hook records its emitted polybuf interval as
 * RP_TRANSITION; the sub-tick reextract gates on
 * render_path_canon_drawn(RP_TRANSITION), which drops the moment
 * func_8004F000 stops emitting. */

/** @brief Re-derives the faded cyclorama dome func_8004F000 draws from gamestate against the active frustum.
 *
 * func_8004F000 culls each face by AND-ing its three vertices' outcodes
 * against the 4:3 box, so its prims stop at the 4:3 edges;
 * cyclorama_native_extract_faded iterates the whole dome and rejects
 * against the widened bounds instead. Does nothing on a speculative
 * runahead tick, whose results the rollback would not revert.
 * @param cpu The guest CPU state. */
static void crossfade_extract(CPUState* cpu) OP_REQUIRES(sim) {
    /* Runahead lookahead: a speculative tick crossing a level-exit trigger
     * would leave its tris in these host buffers; the rollback does not revert
     * them. The committed re-run re-extracts for real. */
    extern int openpete_speculative;
    if (openpete_speculative) return;
    s_cross_n = cyclorama_native_extract_faded(cpu, s_cross, MAX_DOME_TRI);
}

/** @brief Keep the dome the canonical draw just extracted for a held camera cut.
 *
 * Does nothing on a speculative runahead tick, which extracted nothing. */
static void crossfade_capture(void) OP_REQUIRES(sim) {
    extern int openpete_speculative;
    if (openpete_speculative) return;
    cut_held_sets_capture(&s_sets);
    memcpy(s_tick, s_cross, (size_t)s_cross_n * sizeof *s_cross);
    s_tick_n = s_cross_n;
}

/** @brief Feeds the extracted cross-fade dome to the native scene as the backmost world layer.
 *
 * The dome sits at view-Z 4096 * 0.999 / 0.001, short of the far clear, so
 * the balloon moby and HUD text draw over it as on the PSX; outside it the
 * draw-env clear shows. The dome's vertex colours carry the DPCS fade func_8004F000
 * applies.
 * @param tris The dome's triangles.
 * @param n Their count. */
static void crossfade_feed_native(cyclorama_native_tri* tris, int n) OP_REQUIRES(sim) {
    if (n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);

    /* Far backdrop, still < 1.0 clear: view-Z 4096 * 0.999 / (1 - 0.999) in
     * float. */
    const float depth = 0x1.f381a6p+21f;
    for (int i = 0; i < n; i++) {
        const cyclorama_native_tri* t = &tris[i];
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = t->sx[k]; st.v[k].sy = t->sy[k]; st.v[k].z = depth;
            st.v[k].pvz = depth;
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (t->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 0;                                      /* the dome is opaque F3/G3 */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
}

/* ---- gating + diagnostics ------------------------------------------------- */
/** @brief Reports whether OPENPETE_TRANSITION_PROBE enables the transition hook probe log.
 *
 * Read once and cached; set means non-empty and not starting with '0'.
 * @return 1 when enabled, else 0. */
static int probe_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRANSITION_PROBE"); v = (e && *e && *e != '0'); }
    return v;
}
/** @brief Reports whether OPENPETE_TRANSITION_DUMP enables the cross-fade prim dump.
 *
 * Read once and cached; set means non-empty and not starting with '0'.
 * @return 1 when enabled, else 0. */
static int dump_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TRANSITION_DUMP"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief Logs one red probe line per hooked function per openpete_frame value when the probe is enabled.
 *
 * A transition spanning many ticks logs a burst that brackets it. The
 * logged ra names the caller: 0x8001C6xx is GS_ExitLevel (func_8001C694),
 * 0x8001E2xx is GS_Balloonist (func_8001E24C).
 * @param name The hooked function's name.
 * @param what What the function draws.
 * @param cpu The guest CPU state.
 * @param last The caller's last-logged openpete_frame, updated on a log. */
static void probe_log(const char* name, const char* what, CPUState* cpu, unsigned* last) {
    if (!probe_on()) return;
    if (*last == openpete_frame) return;
    *last = openpete_frame;
    fprintf(stderr, CRED "[TRANSITION-PROBE f=%u] %s  %s  (ra=0x%08x)" CRESET "\n",
            openpete_frame, name, what, cpu->ra);
}

/* ---- cross-fade hook (func_8004F000): run the real renderer, capture + feed ---- */
/** @brief Hook on func_8004F000 that runs the cross-fade renderer, records its polybuf range and feeds the re-derived dome natively.
 *
 * Inert (returns 0) when neither the native scene nor render paths are
 * active, and on re-entry. The
 * range is recorded as RP_TRANSITION, which drives the OT-walk filter and
 * the canon-drawn gate transition_reextract reads; an empty range reads as
 * not drawn. With the native feed on and a non-empty range, the dome is
 * extracted and fed; OPENPETE_TRANSITION_DUMP logs the first range's raw
 * words and the first extraction's extent.
 * @param cpu The guest CPU state.
 * @param addr Unused.
 * @return 1 when the hook ran the renderer, 0 to let normal dispatch run it. */
static int on_8004F000_crossfade(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    (void)addr;
    int feed = openpete_native_scene_enabled();
    static unsigned last_probe = 0xFFFFFFFFu;
    probe_log("func_8004F000", "edge-fill/cross-fade (GS_ExitLevel + GS_Balloonist)", cpu, &last_probe);
    if (!feed && !render_paths_active()) return 0;        /* inert: normal dispatch */

    static int s_in = 0;
    if (s_in) return 0;                                   /* re-entrancy guard */
    s_in = 1;

    uint32_t startp = cpu->read_word(POLYBUF_CURSOR);
    { OP_PROF_FLAGS_ACCUM_SCOPE("transition.srend", OP_PROF_F_GUEST); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_GUEST);
      func_8004F000(cpu); }                               /* real renderer (verdict + g_SonyImage) */
    uint32_t endp = cpu->read_word(POLYBUF_CURSOR);

    /* Record the emitted polybuf range: feeds the OT-walk isolation filter
     * AND the canon-drawn mask - the staleness gate the sub-tick replay reads
     * (render_path_canon_drawn(RP_TRANSITION), latched once per canonical
     * present by the shared render_paths commit; empty ranges are dropped, so
     * a frame where the cross-fade emitted nothing reads "not drawn"). */
    render_path_record(RP_TRANSITION, startp, endp);

    if (feed && endp > startp) {
        crossfade_extract(cpu);
        crossfade_capture();
        render_material_level_push(cpu);
        crossfade_feed_native(s_cross, s_cross_n);
        if (dump_on()) {
            static int s_rawdumped = 0;
            if (!s_rawdumped) {
                fprintf(stderr, CRED "[transition-RAW f=%u] [%08X,%08X) %u bytes:" CRESET "\n",
                        openpete_frame, startp, endp, endp - startp);
                uint32_t q = startp; int row = 0;
                while (q < endp && row < 24) {
                    fprintf(stderr, "  %08X:", q);
                    for (int c = 0; c < 4 && q < endp; c++, q += 4) fprintf(stderr, " %08X", cpu->read_word(q));
                    fprintf(stderr, "\n"); row++;
                }
                s_rawdumped = 1;
            }
            static int s_dumped = 0;
            if (!s_dumped && openpete_frame >= 1) {
                float minx=1e9f,maxx=-1e9f,miny=1e9f,maxy=-1e9f;
                for (int i = 0; i < s_cross_n; i++)
                    for (int k=0;k<3;k++){
                        float X=s_cross[i].sx[k],Y=s_cross[i].sy[k];
                        if(X<minx)minx=X;
                        if(X>maxx)maxx=X;
                        if(Y<miny)miny=Y;
                        if(Y>maxy)maxy=Y;
                    }
                /* The .s's own prim bytes are the 4:3-culled subset; the extent
                 * below is the widened re-derivation actually fed. */
                fprintf(stderr, CRED "[transition-dump f=%u] polybuf %u bytes -> %d dome tris  "
                        "extent x[%.0f,%.0f] y[%.0f,%.0f]" CRESET "\n",
                        openpete_frame, endp - startp, s_cross_n, minx, maxx, miny, maxy);
                for (int i = 0; i < s_cross_n && i < 4; i++)
                    fprintf(stderr, "    tri[%d] (%.0f,%.0f)=%06X (%.0f,%.0f)=%06X (%.0f,%.0f)=%06X\n", i,
                            s_cross[i].sx[0],s_cross[i].sy[0],s_cross[i].rgb[0],
                            s_cross[i].sx[1],s_cross[i].sy[1],s_cross[i].rgb[1],
                            s_cross[i].sx[2],s_cross[i].sy[2],s_cross[i].rgb[2]);
                s_dumped = 1;
            }
        }
    }
    s_in = 0;
    return 1;
}

/** @brief Hook on GS_ExitLevel (func_8001C694) that drives the native grayscale freeze and runs the game renderer.
 *
 * Inert (returns 0) when neither the native scene nor render paths are
 * active, and on re-entry. Case 1 (D_GS_EXIT_MODE) is the cyclorama
 * fade-in; its clear reaches the native scene through the env its
 * PutDrawEnv applies before the DrawOTag. Case 0 is the green-tinted
 * grayscale freeze of the level being exited: its POLY_FT4s go through the
 * unhooked world-OT add func_800168A0, so before the game renderer runs the
 * hook drives openpete_native_scene_set_freeze_backdrop with tint
 * (0x4C, 0x80, 0x40) and a fade of the entry D_GS_EXIT_TICK scaled from
 * 0..31 to 0..127, the value the call's grayscale CLUT load uses.
 * @param cpu The guest CPU state.
 * @param addr Unused.
 * @return 1 when the hook ran the renderer, 0 to let normal dispatch run it. */
static int on_8001C694(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    (void)addr;
    int feed = openpete_native_scene_enabled();
    static unsigned last = 0xFFFFFFFFu;
    probe_log("func_8001C694", "GS_ExitLevel grayscale freeze + cross-fade", cpu, &last);
    if (!feed && !render_paths_active()) return 0;        /* inert: normal dispatch */

    static int s_in = 0;
    if (s_in) return 0;                                   /* re-entrancy guard */
    s_in = 1;

    if (feed && cpu->read_word(D_GS_EXIT_MODE) == 0) {    /* case 0: grayscale freeze */
        /* Fade the frozen level to black over the 32-tick case, matching PSX: the
         * game shrinks the grayscale CLUT load each tick, so a frozen pixel of luma-
         * index i samples palette[i-tick] (subtractive shift). The CLUT load this
         * call issues uses D_8007568C as it stands at entry. The call submits its
         * OT and then waits its VSyncs, where the host presents, so the freeze is
         * set here, before the call, to be part of that present's scene.
         * tick 0..31 -> shift = tick/31, packed as 0..127 for set_freeze_backdrop. */
        uint32_t tick = cpu->read_word(D_GS_EXIT_TICK);
        if (tick > 31) tick = 31;
        int fade = (int)((tick * 127u + 15u) / 31u);      /* 0..31 -> 0..127 (rounded) */
        render_material_level_push(cpu);
        openpete_native_scene_begin_frame(openpete_frame);
        openpete_native_scene_set_freeze_backdrop(0x4C, 0x80, 0x40, fade);
    }

    { OP_PROF_FLAGS_ACCUM_SCOPE("transition.srend", OP_PROF_F_GUEST); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_GUEST);
      func_8001C694(cpu); }                               /* real renderer (case 1 nests func_8004F000) */
    s_in = 0;
    return 1;
}

/* Sub-tick re-extract of the cross-fade dome and backdrop; see reextract.h. */
void transition_reextract(CPUState* cpu, int hold) OP_REQUIRES(sim) {
    const int pick = cut_held_sets_enter(&s_sets, hold);
    const int ready = hook_reextract_ready_held(RP_TRANSITION, pick);  /* scene on, selected, drawn */
    if (ready) {
        if (pick < 0) crossfade_extract(cpu);
        render_material_level_push(cpu);
        crossfade_feed_native(pick < 0 ? s_cross : s_tick, pick < 0 ? s_cross_n : s_tick_n);
    }
    cut_held_sets_leave(&s_sets, pick);
    if (!ready) return;
    {   /* OPENPETE_LOAD_CENSUS: replay-fire census (liveness-gate verification). */
        static int cen = -1;
        if (cen < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); cen = (e && *e && *e != '0'); }
        static unsigned s_fires = 0; s_fires++;
        static unsigned last = 0xFFFFFFFFu;
        if (cen && openpete_frame / 100u != last) { last = openpete_frame / 100u;
            fprintf(stderr, "[census f=%u TRANSITION-REPLAY] fires=%u\n", openpete_frame, s_fires); }
    }
}

/** @brief Feature-module option hook of the transition path; it reads no option.
 *
 * @param opts Unused. */
static void transition_native_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
}

static const feature_dispatch_entry_t transition_dispatch[] = {
    { 0x8004F000u, on_8004F000_crossfade, 0u },
    { 0x8001C694u, on_8001C694, 0u },
    { 0u, NULL, 0u },
};

const feature_module_t transition_module = {
    .name           = "transition_native",
    .apply_options  = transition_native_apply_options,
    .dispatch_table = transition_dispatch,
};
