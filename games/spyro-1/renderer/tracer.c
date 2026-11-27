// games/spyro-1/renderer/tracer.c - native gamestate re-derivation of the additive "tracer/beam"
// renderer (func_800189F0 -> func_800189F8), e.g. the dream-weavers fireball's
// glowing streak. Re-derived in FLOAT from the tracer-point gamestate (no
// capture of the .s's already-projected int16 prims), so it has true per-vertex
// view-Z depth, high-FPS sub-tick support, and no quantization.
//
// Tracer data (drained + zeroed by the .s every frame, so snapshot at hook
// entry before the real renderer runs):
//   0x80075684            = active tracer-SET count (then zeroed)
//   0x800772C8 + set*4     = per-set point count
//   0x80078658 + set*4     = per-set points-base pointer
//   0x8007596C             = g_LevelId (==63 Gnasty Gnorc boss: width>>2 + R<->G
//                            colour swap, i.e. the white->red gradient -> green)
//   each point = 28-byte record: world XYZ @+0/+4/+8, fade @+24
//     (+12/+16/+20 are the .s's int16 SX/SY/viewZ - we re-project instead).
//
// Algorithm (mirrors func_800189F8, all float):
//   project each point via func_80017B48's permuted/negated axes
//     V = (camY-Y, camZ-Z, X-camX); MAC = camRot * V / 4096; viewZ = MAC3;
//     SX = OFX + MAC1*H/MAC3, SY = OFY + MAC2*H/MAC3   (pure float, no GTE clamp)
//   per point i (fade f, skipped when f>=81):
//     width   = (625*i)<<5 for i<3, 0 at i==count-2, else 0x11170
//               (>>2 if g_LevelId==63)
//     dir     = next point screen delta (dx,dy); len = hypot(dx,dy)
//     perp    = (dy,-dx) * width / (viewZ*len)        (perspective ribbon)
//     corners = P +/- perp ; colour = (150-2f, 150-4f, 150-10f) clamped,
//               R<->G swapped at g_LevelId==63 (Gnasty's green tracers)
//   emit a gouraud G4 (additive, ABR1) spanning prev corners -> curr corners,
//   once >=2 points emitted and viewZ < 256000.
//
// Display-only: the real renderer still runs (verdict/state), we read only the
// tracer-point gamestate and feed the native scene. Oracle inert.

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "feature_registry.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "reextract.h"   /* sub-tick re-extract entry */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "spyro-1/gen/spyro1_symbols.gen.h"
#include "tracer_snapshot.h"
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE - fiber-hook cost split */
#include "openpete_thread_roles.h"

extern void     func_800189F0(CPUState* cpu);   /* the real tracer/beam renderer */
extern unsigned openpete_frame;


static tracer_set_t s_sets[TRACER_MAX_SETS];
static int s_nsets;                  /* sets captured at the last canonical frame */

/** @brief Projects a world point to screen position and full-scale view-Z with func_80017B48's axes, in float.
 *
 * The camera-relative vector is (camY - Y, camZ - Z, X - camX) against the
 * integer camera position the game loads; it is rotated by the camera
 * matrix and divided by view-Z with no GTE int16 clamp or divide cap.
 * @param v The projection view.
 * @param X The point's world x.
 * @param Y The point's world y.
 * @param Z The point's world z.
 * @param sx Receives the screen x, 0 when the point is not projected.
 * @param sy Receives the screen y, 0 when the point is not projected.
 * @param vz Receives the view-Z, written in both cases.
 * @return 1 when view-Z exceeds 1, else 0. */
static int tracer_project(const ngte_view* v,
                         int X, int Y, int Z,
                         double* sx, double* sy, double* vz) {
    const double* cam = v->cam_ram;   /* integer camera, as the .s loads it */
    double V0 = cam[1] - Y;   /* camY - Y */
    double V1 = cam[2] - Z;   /* camZ - Z */
    double V2 = X - cam[0];   /* X - camX */
    double m1, m2, m3;
    ngte_mac(v->cam_M, V0, V1, V2, &m1, &m2, &m3);
    *vz = m3;
    if (m3 <= 1.0) { *sx = *sy = 0.0; return 0; }   /* at/behind camera */
    *sx = v->ofx + m1 * v->H_eff / m3;
    *sy = v->ofy + m2 * v->H_eff / m3;
    return 1;
}

/** @brief Adds one untextured additive gouraud triangle to the native scene.
 *
 * Vertices are screen-space position plus view-Z; each vertex colour is the
 * given 0xBBGGRR value with alpha forced to 0xFF, and the per-vertex view-Z
 * equals the depth. The blend mode is 2 (ABR mode 1, additive).
 * @param ax Vertex a screen x.
 * @param ay Vertex a screen y.
 * @param az Vertex a scene view-Z.
 * @param acol Vertex a colour, 0xBBGGRR.
 * @param bx Vertex b screen x.
 * @param by Vertex b screen y.
 * @param bz Vertex b scene view-Z.
 * @param bcol Vertex b colour, 0xBBGGRR.
 * @param cx Vertex c screen x.
 * @param cy Vertex c screen y.
 * @param cz Vertex c scene view-Z.
 * @param ccol Vertex c colour, 0xBBGGRR. */
static void emit_tri(double ax, double ay, double az, uint32_t acol,
                     double bx, double by, double bz, uint32_t bcol,
                     double cx, double cy, double cz, uint32_t ccol) OP_REQUIRES_SHARED(sim) {
    openpete_scene_tri st = {0};
    memset(&st, 0, sizeof st);
    st.textured = 0;
    st.blend    = 2;                          /* ABR mode 1 (additive) */
    st.v[0].sx = (float)ax; st.v[0].sy = (float)ay; st.v[0].z = (float)az; st.v[0].rgba = acol | 0xFF000000u;
    st.v[1].sx = (float)bx; st.v[1].sy = (float)by; st.v[1].z = (float)bz; st.v[1].rgba = bcol | 0xFF000000u;
    st.v[2].sx = (float)cx; st.v[2].sy = (float)cy; st.v[2].z = (float)cz; st.v[2].rgba = ccol | 0xFF000000u;
    for (int k = 0; k < 3; k++) st.v[k].pvz = st.v[k].z;   /* true per-vertex view-Z */
    openpete_native_scene_add_tri(&st, OPENPETE_SCENE_ACTOR, 0);  /* category ignored for blend!=0 */
}

/** @brief Projects the snapshot tracer points against the current camera and feeds each set's additive ribbon to the native scene.
 *
 * Mirrors func_800189F8 in float. On a sub-tick re-extract the current
 * camera is the interpolated one. Per point i below count-1 with fade
 * below 81, the half-width ramps (625*i)<<5 over the first three points,
 * is 0x11170 in the bulk and 0 at count-2 (quartered on level 63), and is
 * scaled by 1 / (view-Z * screen length) perpendicular to the direction to
 * point i+1. Colour is (150-2f, 150-4f, 150-10f) clamped at 0, with red and
 * green swapped on level 63. From the second emitted point on, while
 * view-Z is below 256000, two additive triangles join the previous and
 * current corner pairs, each vertex at its own view-Z.
 * @param cpu The guest CPU state. */
static void tracer_build_and_feed(CPUState* cpu) OP_REQUIRES(sim) {
    if (s_nsets <= 0) return;
    ngte_view view; ngte_view_build(cpu, &view);   /* one projection view per feed */
    const ngte_view* v = &view;
    int is_gnasty = (cpu->read_word(G_LEVELID) == 63u);   /* level 63: thin + green */
    int wshift    = is_gnasty ? 2 : 0;
    openpete_native_scene_set_tag(OPENPETE_TAG_TRACER(OPENPETE_TAG_TRACER_RIBBON, 0));

    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);

    for (int s = 0; s < s_nsets; s++) {
        tracer_set_t* ts = &s_sets[s];
        int    havePrev = 0, emitted = 0;
        double pc0x=0,pc0y=0,pc1x=0,pc1y=0,pvz=0; uint32_t pcol = 0;
        /* The .s ribbon loop runs s6 = 0..count-2 (block_80018A9C requires
         * count>=2; block_80018EB4 continues while s6 < count-1). The last
         * point (count-1) is projected in the pre-pass but only consumed as the
         * direction target for point count-2's segment - never drawn. Point
         * count-2 takes width 0 (the tip pinch), so the final segment tapers to
         * a point. Iterating to count-1 here would emit an extra full-width
         * segment whose reversed neighbour direction crosses over past the tip. */
        for (int i = 0; i < ts->count - 1; i++) {
            int f = ts->pt[i].f;
            if (f >= 81) continue;            /* fully faded -> skip (prev unchanged) */
            double sx, sy, vz;
            if (!tracer_project(v, ts->pt[i].x, ts->pt[i].y, ts->pt[i].z, &sx, &sy, &vz))
                continue;
            /* screen direction to the next point (i < count-1, so it exists) */
            int j = (i + 1 < ts->count) ? i + 1 : i - 1;
            double dx = 0.0, dy = 0.0;
            if (j >= 0) {
                double nx, ny, nvz;
                if (tracer_project(v, ts->pt[j].x, ts->pt[j].y, ts->pt[j].z, &nx, &ny, &nvz)) {
                    dx = nx - sx; dy = ny - sy;
                }
            }
            double len = sqrt(dx*dx + dy*dy);
            /* Ribbon half-width (func_800189F8 block_80018AC8/AFC): the first 3
             * points ramp via the per-point accumulator ((625*i)<<5 = 0/20000/
             * 40000); the bulk is a constant 0x11170 (70000); the second-to-last
             * point is 0 (the ribbon pinches at the tip). (>>2 when cfg==63.) */
            int wi = (i < 3) ? ((625 * i) << 5)
                             : ((i == ts->count - 2) ? 0 : 0x11170);
            if (wshift) wi >>= 2;
            double width = (double)wi;
            double px = 0.0, py = 0.0;
            if (len > 1e-6 && vz > 1.0) {
                double scale = width / (vz * len);       /* perspective ribbon half-width */
                px = dy * scale; py = -dx * scale;
            }
            int Rc = 150 - 2 * f, Gc = 150 - 4 * f, Bc = 150 - 10 * f;
            if (Rc < 0) Rc = 0;
            if (Gc < 0) Gc = 0;
            if (Bc < 0) Bc = 0;
            if (is_gnasty) { int t = Rc; Rc = Gc; Gc = t; }  /* func_800189F8: g_LevelId==63 swaps R<->G */
            uint32_t col = (uint32_t)(Rc | (Gc << 8) | (Bc << 16));
            double c0x = sx + px, c0y = sy + py;
            double c1x = sx - px, c1y = sy - py;
            emitted++;
            if (emitted >= 2 && havePrev && vz < 256000.0) {
                double dz = openpete_native_scene_viewz(openpete_worldvz_full((float)vz));
                double pz = openpete_native_scene_viewz(openpete_worldvz_full((float)pvz));
                /* G4: v0=c0(cur) v1=c1(cur) v2=pc0(prev) v3=pc1(prev) -> tris (0,1,2)(1,3,2) */
                emit_tri(c0x,c0y,dz,col,  c1x,c1y,dz,col,  pc0x,pc0y,pz,pcol);
                emit_tri(c1x,c1y,dz,col,  pc1x,pc1y,pz,pcol, pc0x,pc0y,pz,pcol);
            }
            pc0x=c0x; pc0y=c0y; pc1x=c1x; pc1y=c1y; pvz=vz; pcol=col; havePrev=1;
        }
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Runs the game's tracer renderer func_800189F0 inside the hook bracket.
 *
 * @param cpu The guest CPU state.
 * @param addr Unused. */
static void tracer_run_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim); (void)addr; func_800189F0(cpu); }
/** @brief Bracket pre-step that snapshots the tracer points before the game renderer drains them, when the native feed is on.
 *
 * @param cpu The guest CPU state.
 * @param feed Nonzero when the native feed runs this call.
 * @param verify Unused. */
static void tracer_pre(CPUState* cpu, int feed, int verify) {
    OP_ROLE_ASSERT(sim);
    (void)verify;
    if (feed) s_nsets = tracer_snapshot_take(cpu, TRACER_COUNT, TRACER_CNTTAB, TRACER_BASETAB,
                                             s_sets, TRACER_MAX_SETS);
}

/* The recorded polybuf range drives the OT-filter isolation AND the canon-drawn
 * liveness gate the sub-tick re-extract reads. It is empty when no tracer drew
 * (render_path_record drops empty ranges), so a frame with no tracer reads as
 * "not drawn" and the spine will not replay a stale beam. */
static hook_bracket_state s_tracer_hb;
static const hook_bracket_desc k_tracer_bracket = {
    .path = RP_TRACER, .prof_tag = "tracer.srend",
    .run = tracer_run_draw, .pre = tracer_pre,
    .st = &s_tracer_hb,
};

/** @brief Entry hook for func_800189F0 that snapshots the tracer points and feeds the native ribbon.
 *
 * Runs the real renderer inside the hook bracket, whose pre-step takes the
 * snapshot when the native feed is on and which records the polybuf range
 * the renderer drew. When the bracket reports a feed, the snapshot holds
 * sets and the tracer path is enabled, the ribbon is built from the
 * snapshot and added to the native scene.
 * @param cpu The guest CPU state at the renderer's entry.
 * @param addr The hooked address.
 * @return 0 when the bracket declines and the ordinary dispatch runs the
 *         function; 1 when the bracket ran it. */
static int on_800189F0_tracer(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    uint32_t pb0, pb1; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_tracer_bracket, &pb0, &pb1, &feed, &verify))
        return 0;
    if (feed && s_nsets > 0 && render_path_enabled(RP_TRACER))
        tracer_build_and_feed(cpu);
    return hook_bracket_end(&k_tracer_bracket);
}

void tracer_reextract(CPUState* cpu) OP_REQUIRES(sim) {
    if (!hook_reextract_ready(RP_TRACER)) return;
    tracer_build_and_feed(cpu);
}

static const feature_dispatch_entry_t tracer_dispatch[] = {
    { 0x800189F0u, on_800189F0_tracer, 0u },
    { 0u, NULL, 0u },
};

const feature_module_t tracer_module = {
    .name           = "tracer_native",
    .apply_options  = NULL,
    .dispatch_table = tracer_dispatch,
};
