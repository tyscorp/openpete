/* games/common/renderer/reextract_spyro23.c - the sub-tick re-extract
 * orchestrator Spyro 2 and Spyro 3 share: on every present between two
 * canonical ticks it installs the camera transform interpolated between the
 * interp ring's prev and curr snapshots (region 0) into the sub-tick view, and
 * re-runs the sky, terrain, portal, moby, player and effect feeders against
 * it. The staged bytes are what every feeder read resolves to for the duration
 * of the present; guest RAM is never written.
 *
 * Provides the renderer/orchestrator.h entries through spyro23_orchestrator.
 * Every engine address it reads comes from game_layout. A moby cull queue the
 * last canonical present did not capture is never staged, so the moby replay
 * cannot decode a previous state's records. The per-producer camera bracket
 * reports "nothing captured": every producer draws under the tick-end
 * camera. */
#include "renderer/orchestrator.h"
#include "renderer/cyclorama.h"
#include "renderer/moby.h"
#include "renderer/terrain.h"
#include "renderer/player.h"
#include "renderer/player_ranges.h"   /* the player hook's scratch ranges */
#include "renderer/shadow.h"        /* shadow_reextract, shadow_player_reextract */
#include "renderer/particles.h"
#include "renderer/player_game.h"   /* PLAYER_PARTS_MAX */
#include "renderer/portal_spyro23.h" /* portal_s23_reextract */
#include "openpete_psx.h"          /* openpete_psx_to_host_span */
#include "openpete_interp.h"
#include "openpete_profile.h"          /* the re-extract's per-producer regions */
#include "renderer/flame.h"          /* the shared fire-breath producer */
#include "renderer/tracers.h"
#include "renderer/glow.h"
#include "renderer/glint.h"
#include "renderer/zap.h"
#include "renderer/interp_lerp.h"    /* lerp16/32, lerp_matrix, lerp_vec3 */
#include "renderer/interp_regions_gen.h"
#include "renderer/cut_guard.h"      /* the shared camera-cut guard and its census */
#include "renderer/reextract_view.h"
#include "renderer/env_knob.h"
#include "openpete_interp_objs.h"      /* g_openpete_tick_period_ns */
#include "openpete_native_render.h"
#include "openpete_subtick_view.h"
#include "openpete_pace_log.h"
#include "openpete_fiber.h"   /* openpete_frame */
#include "psx_runtime_internal.h"
#include "game_layout.h"
#include "freecam_game.h"            /* freecam_present_pose: a fractional flight path's curve */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

/* Offsets inside the camera transform block, all confirmed against the layout
 * descriptor's read sites in Spyro 2's asm: the sky matrix the cyclorama draw loads with ctc2
 * (cyclorama.s:32-40), the world matrix the terrain draw loads the same way
 * (environment.s:1572-1581) and the world position the sector cull reads
 * (environment.s:1585-1588). */
#define CAM_OFF_SKY   0x00u
#define CAM_OFF_VIEW  0x14u
#define CAM_OFF_POS   0x28u
/* The world position inside the player-shadow state block
 * (game_layout.player.shadow_base; Spyro 2 asm/mobyfunc.s:626-628, Spyro 3
 * asm/mobyfunc.s:630-632). */
#define SHADOW_POS_OFF 0x04u

/* The un-rounded interpolated camera position of this present, for the
 * projection's floor fix; valid only while a bracket is installed. */
static int    s_fpos_active;
static double s_fpos[3];

/* The same for the player's world position, installed per record by the replay
 * loop below. The provider's part-0 transform asks for it by the state block's
 * address, so one slot serves every record in turn. */
static int    s_ppos_active;
static double s_ppos[3];

/** @brief Return the un-rounded interpolated position of the camera or the player the present in flight installed (openpete_native_interp_pos). */
static int orch_interp_pos(unsigned int key, double out[3]) {
    const double* src = NULL;
    if (s_fpos_active && key == game_layout.cam.base)          src = s_fpos;
    else if (s_ppos_active && key == game_layout.player.base)  src = s_ppos;
    if (!src) return 0;
    out[0] = src[0]; out[1] = src[1]; out[2] = src[2];
    return 1;
}

/* The canonical-render epoch (renderer/orchestrator.h): every canonical
 * present closes one, and a producer that ran inside the one just closed is
 * fresh, so a sub-tick pass replays only what the canonical present drew. The
 * player and moby record captures stamp the epoch in flight; the terrain
 * feeder re-derives from the camera and stamps nothing. */
static uint32_t s_epoch_now = 1;   /* the stamp captures take (present in flight) */
static uint32_t s_epoch_ok;        /* the stamp a capture must carry to be fresh */
/* The stamp the moby cull's record capture took, so a tick that ran no moby
 * draw (a pause or a load screen) leaves the queue stale instead of replaying
 * the last level's records. */
static uint32_t s_mobyreg_epoch;
/* Whether that queue is staged over its own address for the present in flight.
 * Only a queue captured in the last closed epoch is staged, so this is the
 * regular record pass's freshness: a present that could not stage the queue
 * must not walk it. */
static int      s_recs_staged;

/** @brief Reports whether OPENPETE_CAM_DBG is set to anything but 0, latched on first call.
 *
 * When set, each canonical present and each sub-tick re-extract prints the
 * camera transform the pass draws with: the world matrix at +0x14 as nine
 * shorts, the position at +0x28 as three words and the sky matrix's first
 * short. The canonical line reads live guest memory; the re-extract lines read
 * live memory, the staged lerp and the ring's two endpoints, so a camera the
 * two passes disagree on is a printed number. Each re-extract also prints a
 * [campose] line: the staged world matrix, the unrounded position the pass
 * draws with, and whether a fractional flight path's pose replaced the lerp.
 * Diagnostic only.
 * @return 1 when the variable is set, else 0. */
static int cam_dbg_on(void) {
    static int v = -1;
    ENV_FLAG_LATCH(v, "OPENPETE_CAM_DBG");
    return v;
}
/** @brief Prints one OPENPETE_CAM_DBG line of a camera transform block to stderr.
 *
 * Prints the position (+0x28, three words), the view matrix (+0x14, nine
 * shorts) and the first short of the sky matrix (+0x00), or "(null)" for a
 * null block.
 * @param tag Label for the line.
 * @param c A camera transform block of at least CAM_OFF_POS + 12 bytes, or
 *        NULL. */
static void cam_dbg_line(const char* tag, const uint8_t* c) {
    if (!c) { fprintf(stderr, "[cam-dbg f=%u %s] (null)\n", openpete_frame, tag); return; }
    const int16_t* m = (const int16_t*)(c + CAM_OFF_VIEW);
    const int16_t* sk = (const int16_t*)(c + CAM_OFF_SKY);
    const int32_t* p = (const int32_t*)(c + CAM_OFF_POS);
    fprintf(stderr, "[cam-dbg f=%u %s] pos=(%d,%d,%d) view=[%d %d %d %d %d %d %d %d %d] sky0=%d\n",
            openpete_frame, tag, p[0], p[1], p[2],
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], sk[0]);
}

/* A row of a snapshot matrix farther than this, in Q12 units, from the row
 * the stamped angles compose refuses the pose override: the matrix was not
 * built from the stamp in the form freecam_view_rotation assumes. */
#define CAM_POSE_TOL_Q12 8.0

/** @brief Install a fractional flight path's pose for this present into the staged camera.
 *
 * Asks the free camera for the path curve's pose at the bracket's alpha
 * (freecam_present_pose). The game builds the sky matrix (+0x00) and the
 * world matrix (+0x14) as a per-row scale times the pose's rotation (Spyro 3
 * stdutil.s func_8004F6C4: the world matrix's row 1 times 0x140 >> 9), so
 * each row's scale is read from the earlier snapshot as its length rounded
 * to Q12, checked against the rotation of the angles stamped for that
 * snapshot, and applied to the curve pose's rotation. Writes both matrices
 * and the rounded position into the staged copy and the unrounded position
 * into @p fpos. Nothing is written when the free camera supplies no pose or
 * any row fails the check.
 * @param cam The staged camera block.
 * @param prev The earlier snapshot of the bracket.
 * @param curr The later snapshot of the bracket.
 * @param pcam The earlier snapshot's camera block.
 * @param ccam The later snapshot's camera block.
 * @param alpha The bracket's blend factor.
 * @param fpos Receives the unrounded camera position.
 * @return 1 when the pose was installed, else 0. */
static int cam_install_path_pose(uint8_t* cam, const openpete_interp_snapshot_t* prev,
                                 const openpete_interp_snapshot_t* curr, const uint8_t* pcam,
                                 const uint8_t* ccam, double alpha, double fpos[3]) {
    double pose[6];
    int32_t pang[3];
    if (!freecam_present_pose(prev->tick, (const int32_t*)(pcam + CAM_OFF_POS), curr->tick,
                              (const int32_t*)(ccam + CAM_OFF_POS), alpha, pose, pang))
        return 0;
    double R[3][3], Rp[3][3];
    freecam_view_rotation(pose[3], pose[4], pose[5], R);
    freecam_view_rotation((double)pang[0], (double)pang[1], (double)pang[2], Rp);
    static const uint32_t offs[2] = { CAM_OFF_SKY, CAM_OFF_VIEW };
    int16_t out[2][9];
    for (int m = 0; m < 2; m++) {
        const int16_t* M = (const int16_t*)(pcam + offs[m]);
        for (int r = 0; r < 3; r++) {
            const double row[3] = { M[3 * r], M[3 * r + 1], M[3 * r + 2] };
            const double sc = round(sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]));
            for (int c = 0; c < 3; c++) {
                if (fabs(row[c] - sc * Rp[r][c]) > CAM_POSE_TOL_Q12) return 0;
                out[m][3 * r + c] = (int16_t)lround(sc * R[r][c]);
            }
        }
    }
    for (int m = 0; m < 2; m++) memcpy(cam + offs[m], out[m], sizeof out[m]);
    int32_t* pos = (int32_t*)(cam + CAM_OFF_POS);
    for (int i = 0; i < 3; i++) {
        fpos[i] = pose[i];
        pos[i] = (int32_t)lround(pose[i]);
    }
    return 1;
}

/** @brief Returns the shared cut guard's descriptor of this game's camera fields, resolving the ring row on first call.
 *
 * The sky matrix (+0x00) decides the rotation tests and the world position
 * (+0x28) the position tests. The camera row index is looked up once and
 * cached in the static descriptor.
 * @return The descriptor, valid for the process lifetime. */
static const cut_cam_desc_t* cut_desc(void) {
    static cut_cam_desc_t d = { -2, CAM_OFF_POS, CAM_OFF_SKY };
    if (d.cam_region == -2) d.cam_region = game_interp_row_index(GAME_INTERP_CAM_ROW);
    return &d;
}

/** @brief Latch the in-flight canonical-render epoch as fresh, start the next and close the player hooks' record epoch (spyro_native_canon_epoch_commit). */
static void orch_epoch_commit(void) {
    OP_ROLE_ASSERT(sim);
    if (cam_dbg_on())
        cam_dbg_line("CANON", (const uint8_t*)openpete_psx_to_host_span(game_layout.cam.base, CAM_OFF_POS + 12u));
    if (cut_census_on()) cut_locate_camera(cut_desc());
    s_epoch_ok = s_epoch_now;
    s_epoch_now++;
    player_records_epoch_commit();               /* close the hooks' record epoch */
}
/** @brief Move the in-flight epoch past the captures of a render no canonical present showed (spyro_native_canon_epoch_supersede). */
static void orch_epoch_supersede(void) { s_epoch_now++; }
/** @brief Return the in-flight canonical-render epoch (spyro_native_canon_epoch_stamp). */
static uint32_t orch_epoch_stamp(void) { return s_epoch_now; }
/** @brief Report whether a capture stamp is the last closed epoch (spyro_native_canon_epoch_fresh). */
static int orch_epoch_fresh(uint32_t stamp) { return stamp == s_epoch_ok; }
/** @brief Report whether the moby cull queue is staged, which implies the last closed epoch captured it (spyro_native_canon_mobyreg_fresh). */
static int orch_mobyreg_fresh(void) { return s_recs_staged; }
/* No second material pass: the shaded producer is never fresh (the inert
 * entry). */

/* The moby cull records live in the draw scratch this tick carved out of the
 * primitive buffer, and the draw passes that follow the moby draw reuse that
 * memory before any sub-tick present runs. So the queue is copied at the moby
 * hook and staged back over its own address for the duration of each
 * intermediate present, which is what makes the replay read the records the
 * tick actually drew rather than whatever overwrote them. */
static uint8_t  s_recs_copy[0x1000];
static uint32_t s_recs_base;
static uint32_t s_recs_len;

/** @brief Capture a render-scratch sub-range into the player range slots (spyro_native_canon_capture_range). */
static void orch_capture_range(void* cpu_v, unsigned off, unsigned len) {
    OP_ROLE_ASSERT(sim);
    player_ranges_capture((CPUState*)cpu_v, off, len);
}
/** @brief Copy the moby cull-record queue at the moby hook for staging on the sub-tick presents (spyro_native_canon_capture_recs). */
static void orch_capture_recs(void* cpu) {
    (void)cpu;
    s_recs_len = 0;
    uint32_t base = moby_canon_rec_base();
    uint32_t len  = moby_canon_rec_bytes();
    if (!base || !len || len > sizeof s_recs_copy) return;
    const void* host = openpete_psx_to_host_span(base, len);
    if (!host) return;
    memcpy(s_recs_copy, host, len);
    s_recs_base = base;
    s_recs_len  = len;
    s_mobyreg_epoch = s_epoch_now;
}

/** @brief Stages the captured moby cull-record queue over its own address for the present in flight.
 *
 * Sets s_recs_staged to the result, which orch_mobyreg_fresh reads. Nothing
 * is staged when no queue was captured, when the capture's epoch is not the
 * last canonical one (a tick that ran no moby draw), or when the stage fails,
 * so the caller skips the replay rather than decoding a previous state's
 * records or whatever now lives at the queue's address.
 * @return 1 when the queue is staged, else 0. */
static int stage_moby_records(void) {
    s_recs_staged = 0;
    if (!s_recs_len) return 0;
    if (s_mobyreg_epoch != s_epoch_ok) return 0;
    void* stage = openpete_subtick_view_stage(s_recs_base, s_recs_len);
    if (!stage) return 0;
    memcpy(stage, s_recs_copy, s_recs_len);
    s_recs_staged = 1;
    return 1;
}

/* The SXY capture and the producer cameras are the inert entries: the player
 * walk decodes the captured part records, not the SXY block, and every
 * producer draws under the tick-end camera. */

/** @brief The player's ground shadow, at its own place in the record order.
 *
 * The shadow feeder reads one state block and the record store holds the block
 * each call drew with, so the loop stages that block over the ambient bytes and
 * runs the fan once per record. When the record lerps, the anchor is
 * bracketed against the paired previous shadow record's and snaps on a move
 * over 5000 world units, so the shadow slides with the avatar between ticks
 * instead of stepping with it. The staged block is the
 * record's shown state (`src`, the led tick's under --runahead).
 *
 * Host-only: the staged span is a view copy and no guest word is written. */
static void player_record_shadow(CPUState* cpu, int k, uint8_t* shb, double rec_alpha) OP_REQUIRES(sim) {
    if (!shb) return;
    const uint32_t len = game_layout.player.shadow_len;
    int nsh = player_records_shadow_count();
    for (int j = 0; j < nsh; j++) {
        const player_shadow_rec_t* d = player_records_shadow_curr(j);
        if (!d || d->model != k || !d->drawn) continue;
        const player_shadow_rec_t* pd = player_records_shadow_prev(j);
        memcpy(shb, d->src, len);
        if (player_records_lerps(k) && pd) {
            const int32_t* pp = (const int32_t*)(pd->src + SHADOW_POS_OFF);
            const int32_t* cp = (const int32_t*)(d->src + SHADOW_POS_OFF);
            double dx = (double)cp[0] - pp[0], dy = (double)cp[1] - pp[1], dz = (double)cp[2] - pp[2];
            if (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0)
                lerp_vec3(shb, pd->src, d->src, SHADOW_POS_OFF, rec_alpha);
        }
        shadow_player_reextract(cpu, k);
        player_records_shadow_note(j, (const int32_t*)(shb + SHADOW_POS_OFF));
    }
}

/** @brief Replay every player call the canonical tick recorded, not just the avatar.
 *
 * The pass driver draws the avatar twice on a tick over a reflective surface:
 * a mirrored copy first, with the height above the surface in the state word,
 * the world Z moved to twice that height below it and its own ordering bias
 * in the sort byte, then the avatar with those three fields restored
 * (the Spyro 3 decomp's asm/nonmatchings/drawutil/func_8001EC5C.s:39-90).
 * Live gamestate at present time therefore describes neither call: the driver
 * put it back. The record store holds each call's own state block (its shown
 * state: the led tick's under --runahead), so the loop stages that block into
 * the sub-tick view and runs the walk once per record, with the mirrored
 * verdict taken from the record rather than re-read. Reproducing the copy from
 * the capture is what makes it draw on an intermediate present; without it the
 * reflection appears only on canonical ticks and vanishes at any present rate
 * above the tick rate.
 *
 * Host-only: the staged span is a view copy, restored to the ambient bytes at
 * the end, and no guest word is written. */
static void player_records_replay(CPUState* cpu, int paired, double alpha) OP_REQUIRES(sim) {
    const uint32_t len = game_layout.player.rec_len;
    double rec_alpha = 0.0;
    int n = player_records_present_begin(paired, 0, alpha, &rec_alpha);
    if (n <= 0 || !len || len > PLAYER_REC_STATE_MAX) {
        player_records_present_end(); return;
    }
    uint8_t* sb = (uint8_t*)openpete_subtick_view_stage(game_layout.player.base, len);
    if (!sb) { player_records_present_end(); return; }

    static uint8_t ambient[PLAYER_REC_STATE_MAX];
    memcpy(ambient, sb, len);

    /* The shadow feeder's own state block, staged beside the player's for the
     * same reason: one block serves every record in turn. */
    const uint32_t shlen = game_layout.player.shadow_len;
    uint8_t* shb = NULL;
    static uint8_t sh_ambient[PLAYER_REC_AUX_MAX];
    if (game_layout.player.shadow_base && shlen && shlen <= PLAYER_REC_AUX_MAX &&
        player_records_shadow_count() > 0) {
        shb = (uint8_t*)openpete_subtick_view_stage(game_layout.player.shadow_base, shlen);
        if (shb) memcpy(sh_ambient, shb, shlen);
    }
    const uint32_t pos_off = game_layout.player.pos_off;
    for (int k = 0; k < n; k++) {
        const player_model_rec_t* rec = player_records_curr(k);
        if (!rec) break;
        if (!rec->drawn) { player_records_present_note(k, (const int32_t*)rec->src); continue; }
        memcpy(sb, rec->src, len);

        /* Position and rotation between the two ticks this record pairs.
         * Without it the avatar holds its tick pose while the camera glides,
         * so every present above the tick rate shows the same 30 Hz step the
         * camera bracket exists to remove. The record store pairs each call
         * with its own previous one, so a mirrored copy brackets against the
         * previous tick's mirrored copy rather than against the avatar.
         *
         * The position goes in twice: as bytes, for the provider reads that
         * take the state block (the water plane, the mirrored copy's height),
         * and as the un-rounded double the part-0 transform prefers, which is
         * what keeps the avatar off the one-world-unit grid.
         *
         * Rotation is handed over as float byte-angles rather than written
         * back into the packed euler, because the euler is 8 bits per axis:
         * rounding the interpolated angle into it reproduces the 1.4-degree
         * step it is meant to subdivide. The shortest arc is taken per axis,
         * so a wrap through 0 does not spin the model the long way. */
        const player_model_rec_t* prv = player_records_prev(k);
        const int lerp = player_records_lerps(k) && prv != NULL;
        int    rot_active[PLAYER_PARTS_MAX] = {0};
        double rot_ang[PLAYER_PARTS_MAX][3] = {{0}};
        s_ppos_active = 0;
        if (lerp) {
            const uint8_t* psp = prv->src;
            const uint8_t* csp = rec->src;
            lerp_vec3(sb, psp, csp, pos_off, rec_alpha);
            const int32_t* pp = (const int32_t*)(psp + pos_off);
            const int32_t* cp = (const int32_t*)(csp + pos_off);
            for (int i = 0; i < 3; i++)
                s_ppos[i] = (double)pp[i] + ((double)cp[i] - (double)pp[i]) * rec_alpha;
            s_ppos_active = 1;
            for (int s = 0; s < PLAYER_PARTS_MAX; s++) {
                uint32_t off = game_layout.player.rot_off[s];
                if (!off) continue;
                uint32_t pr, cr;
                memcpy(&pr, psp + off, 4); memcpy(&cr, csp + off, 4);
                /* A zero word is the provider's "this part does not rotate
                 * this call" sentinel; interpolating through it would swing
                 * the part to the identity and back. */
                if (s != 0 && (pr == 0 || cr == 0)) continue;
                for (int a = 0; a < 3; a++) {
                    int pbk = (int)((pr >> (a * 8)) & 0xFFu);
                    int cbk = (int)((cr >> (a * 8)) & 0xFFu);
                    int d = cbk - pbk;
                    if (d > 128) d -= 256; else if (d < -128) d += 256;
                    rot_ang[s][a] = (double)pbk + (double)d * rec_alpha;
                }
                rot_active[s] = 1;
            }
        }
        player_native_set_rot_interp(lerp ? rot_active : NULL, &rot_ang[0][0]);
        /* The keyframe pose, on the same bracket as the position and rotation
         * above. The part records are composed from gamestate, and the record
         * store holds each call's composed block, so the walk decodes the two
         * poses the two ticks really rendered and lerps the projected
         * vertices. Without it the skeleton translates and turns smoothly
         * while its vertices hold the tick's keyframe blend, so the outline
         * steps at the tick rate under a smooth centroid. An unbracketed
         * present or an unpaired record leaves it off, which draws the tick's
         * own pose. */
        player_native_set_anim_interp(rec_alpha, lerp && paired);
        player_native_set_anim_pair((lerp && prv->drawn) ? prv->seg : NULL, rec->seg);
        player_native_set_player_slot(k);
        player_reextract_record(cpu, !rec->is_player);
        player_record_shadow(cpu, k, shb, rec_alpha);
        player_records_present_note(k, (const int32_t*)sb);
    }
    if (shb) memcpy(shb, sh_ambient, shlen);
    s_ppos_active = 0;
    player_native_set_rot_interp(NULL, NULL);
    player_native_set_anim_pair(NULL, NULL);
    player_native_set_anim_interp(0.0, 0);
    memcpy(sb, ambient, len);
    player_records_present_end();
}

/** @brief Reports whether openpete_frame lies in the OPENPETE_RX_CENSUS range (LO-HI, or one value).
 *
 * While it does, each sub-tick re-extract prints one line of the triangles
 * each producer added to the scene, in call order. A producer that returns
 * early reads as zero, which separates a missing producer from an empty one.
 * The range is read once. Diagnostic only.
 * @return 1 inside the range, else 0. */
static int rx_census_on(void) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    return env_frame_range_active(&s_range, "OPENPETE_RX_CENSUS", openpete_frame);
}
/** @brief Counts the triangles currently in the native scene across all tag families.
 *
 * @return The sum of the 16 family counts. */
static uint32_t rx_scene_tris(void) OP_REQUIRES_SHARED(sim) {
    uint32_t n[16], t = 0;
    openpete_native_scene_tag_family_counts(n);
    for (int f = 0; f < 16; f++) t += n[f];
    return t;
}
/* The line is assembled in a buffer and written once at END: the producers
 * this census brackets have stderr traces of their own, and a line built by
 * one fprintf per step interleaves with them into an unreadable row. */
#define RX_CENSUS_BEGIN() \
    const int rx_cen = rx_census_on(); \
    uint32_t rx_prev = 0; \
    char rx_line[256]; int rx_len = 0; \
    if (rx_cen) { rx_prev = rx_scene_tris(); \
        rx_len = snprintf(rx_line, sizeof rx_line, "[rx-census f=%u REEXTRACT]", openpete_frame); \
        if (rx_len < 0) rx_len = 0; }
#define RX_CENSUS_STEP(nm) \
    if (rx_cen) { uint32_t rx_now = rx_scene_tris(); \
        if (rx_len < (int)sizeof rx_line) { \
            int rx_n = snprintf(rx_line + rx_len, sizeof rx_line - (size_t)rx_len, \
                                " %s=%u", (nm), rx_now - rx_prev); \
            if (rx_n > 0) rx_len += rx_n; } \
        rx_prev = rx_now; }
#define RX_CENSUS_END(br, al) \
    if (rx_cen) { \
        if (rx_len < (int)sizeof rx_line) \
            snprintf(rx_line + rx_len, sizeof rx_line - (size_t)rx_len, \
                     " | bracketed=%d alpha=%.3f", (br), (al)); \
        rx_line[sizeof rx_line - 1] = '\0'; \
        fprintf(stderr, "%s\n", rx_line); }

/* The camera a present draws with: the shared bracket's verdict and whether
 * a fractional flight path's pose replaced its lerp. */
typedef struct {
    cam_bracket_t br;
    int           path_pose;
} rx_camera_t;

/** @brief Stage the camera a sub-tick present draws with.
 *
 * Runs the shared camera bracket (cam_bracket_run) into the staged camera
 * block, installs its unrounded position for openpete_native_interp_pos,
 * and, outside a cut or a held cut, lets a fractional flight path replace
 * the lerp with its curve (cam_install_path_pose). An unpaired present, or
 * one with no staged block or snapshot, stages nothing.
 * @param cam The staged camera block, or NULL.
 * @param paired Non-zero when the ring returned a snapshot pair.
 * @param prev2 The snapshot before prev, or NULL.
 * @param prev The earlier snapshot.
 * @param curr The later snapshot.
 * @return The bracket verdict and the path-pose flag; all zero when nothing was staged. */
static rx_camera_t rx_stage_camera(uint8_t* cam, int paired,
                                   const openpete_interp_snapshot_t* prev2,
                                   const openpete_interp_snapshot_t* prev,
                                   const openpete_interp_snapshot_t* curr) OP_REQUIRES(sim) {
    rx_camera_t rc;
    memset(&rc, 0, sizeof rc);
    if (!paired || !cam || !prev || !curr) return rc;
    cam_bracket_run(cam, prev2, prev, curr, cut_desc(), CAM_OFF_VIEW, &rc.br);
    if (!rc.br.bracketed) return rc;
    for (int i = 0; i < 3; i++) s_fpos[i] = rc.br.fpos[i];
    s_fpos_active = 1;
    const int r = cut_desc()->cam_region;
    if (rc.br.cut || rc.br.hold || r < 0) return rc;
    rc.path_pose = cam_install_path_pose(cam, prev, curr, prev->data + prev->region_offsets[r],
                                         curr->data + curr->region_offsets[r], rc.br.alpha, s_fpos);
    return rc;
}

/** @brief Print the OPENPETE_CAM_DBG lines of one sub-tick present.
 *
 * The present's bracket verdict; a [campose] line with the staged world
 * matrix, the unrounded position the pass draws with, whether a fractional
 * flight path's pose replaced the lerp, and the vertical and horizontal FOV;
 * then the camera of live memory, the staged copy and the ring's two
 * endpoints (cam_dbg_line).
 * @param cam The staged camera block, or NULL.
 * @param rc The staged camera's verdict.
 * @param paired Non-zero when the ring returned a snapshot pair.
 * @param prev The earlier snapshot, or NULL.
 * @param curr The later snapshot, or NULL. */
static void cam_dbg_present(const uint8_t* cam, const rx_camera_t* rc, int paired,
                            const openpete_interp_snapshot_t* prev,
                            const openpete_interp_snapshot_t* curr) {
    fprintf(stderr, "[cam-dbg f=%u RX] paired=%d bracketed=%d alpha=%.4f\n",
            openpete_frame, paired, rc->br.bracketed, rc->br.alpha);
    if (cam) {
        const int16_t* m = (const int16_t*)(cam + CAM_OFF_VIEW);
        const int32_t* p = (const int32_t*)(cam + CAM_OFF_POS);
        double fp[3] = { p[0], p[1], p[2] };
        if (s_fpos_active) memcpy(fp, s_fpos, sizeof fp);
        double vfov = 0.0, hfov = 0.0;
        openpete_native_fov_angles(&vfov, &hfov);
        fprintf(stderr, "[campose] vbl=%u key=%u alpha=%.4f cut=%d path=%d "
                        "M=[%d %d %d %d %d %d %d %d %d] pos=(%.4f, %.4f, %.4f) "
                        "vfov=%.3f hfov=%.3f\n",
                openpete_frame, openpete_feed_key_now(), rc->br.alpha, rc->br.cut, rc->path_pose,
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], fp[0], fp[1], fp[2],
                vfov, hfov);
    }
    cam_dbg_line("RX-live", (const uint8_t*)openpete_psx_to_host_span(game_layout.cam.base, CAM_OFF_POS + 12u));
    cam_dbg_line("RX-staged", cam);
    if (!paired || !prev || !curr) return;
    static int cdbg_region = -2;
    if (cdbg_region == -2) cdbg_region = game_interp_row_index(GAME_INTERP_CAM_ROW);
    if (cdbg_region < 0) return;
    cam_dbg_line("RX-prev", prev->data + prev->region_offsets[cdbg_region]);
    cam_dbg_line("RX-curr", curr->data + curr->region_offsets[cdbg_region]);
}

/** @brief Steer the moby occlusion cull from one present's camera bracket: suspend it across a cut, feed the camera pan otherwise.
 *
 * The occlusion cache holds each record's rect as the last present that drew
 * it projected it. Across a cut that present showed the other side's camera
 * (a held cut draws tick N-1's, the presents after it tick N's), so a cached
 * rect lies at the wrong screen position and depth for up to its two-VBL
 * life: culling is suspended for 12 presents, and every record is decoded and
 * its rect rewritten at the new camera before a cached rect decides again. A
 * stale pair replays a cut the presents since have already drawn at the new
 * camera, so it suspends nothing. Every cut zeroes the pan allowance, since
 * its rotation is no pan. A bracketed present outside a cut sets the
 * allowance (moby_native_set_occl_pan) to the pair's per-tick rotation in
 * radians times 400 px, the canonical projection distance H (about 288) plus
 * margin; the consumer scales it by the view's H scale. Without it a cached
 * rect lying offscreen keeps culling its moby for the tick a camera turn
 * brings it on screen. An unbracketed present leaves the allowance unchanged.
 * @param br The present's camera bracket. */
static void rx_occl_steer(const cam_bracket_t* br) {
    if (br->cut) {
        if (!br->stale) openpete_native_occl_suspend(12);
        moby_native_set_occl_pan(0.0f);
    } else if (br->bracketed) {
        moby_native_set_occl_pan((float)(br->rot * (2.0 * M_PI / 4096.0) * 400.0));
    }
}

/** @brief Whether a sub-tick present before alpha 1 shows tick N-1's particle and moby sets: OPENPETE_LATE_SWITCH, default on, read once.
 *
 * 0 shows tick N's set at every alpha, for a same-binary A/B.
 * @return Nonzero when the switch applies. */
static int late_switch_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_LATE_SWITCH"); v = (e && *e) ? (*e != '0') : 1; }
    return v;
}

/** @brief Composes one sub-tick present: brackets the camera between the tick pair, then runs every feeder's re-extract in draw order.
 *
 * @param cpu_v The guest CPUState, as the void pointer game_reextract_view_install's callback takes. */
static void game_reextract_body(void* cpu_v) {
    OP_ROLE_ASSERT(sim);
    CPUState* cpu = (CPUState*)cpu_v;
    double qa = 0.0;
    const openpete_interp_snapshot_t *prev2 = NULL, *prev = NULL, *curr = NULL;
    /* One per-object hold verdict per present, read by every channel. */
    cut_obj_present_begin();
    int paired = openpete_interp_query3(openpete_interp_present_now(), g_openpete_tick_period_ns,
                                        &qa, &prev2, &prev, &curr);
    if (!paired) openpete_pace_note_alpha(0.0, 0);

    const uint32_t cam_len = game_layout.cam.xform_end - game_layout.cam.base;
    uint8_t* cam = (uint8_t*)openpete_subtick_view_stage(game_layout.cam.base, cam_len);
    /* One cut rule: a pair that is not one motion still shows tick N-1 while
     * its interval is open, so the bracket runs with alpha pinned to 0 (the
     * lerp at 0 is prev byte for byte) and the cut lands on the alpha-1
     * present, where every other channel reaches its canonical N value. The
     * shared guard owns the predicate, the hold verdict and the census. */
    const rx_camera_t rc = rx_stage_camera(cam, paired, prev2, prev, curr);
    const int bracketed = rc.br.bracketed;
    const int cam_cut   = rc.br.cut;
    const int hold      = rc.br.hold;
    const double alpha  = rc.br.alpha;
    rx_occl_steer(&rc.br);
    /* The late switch: before alpha 1 the present shows tick N-1's set of
     * particle and moby records (paired records lerp, records gone or reused
     * at tick N hold their N-1 state, records new at tick N are absent), and
     * tick N's set at alpha 1, so membership changes where every other channel
     * reaches tick N. A held cut shows tick N-1's sets already. */
    const int late = bracketed && alpha < 1.0 && !hold && late_switch_enabled();
    /* The tick side of the bracket, kept for the moby replay: the cull composed
     * every animated record against this camera, so the replay needs it to turn
     * those records onto the interpolated one. */
    const uint8_t* tickcam = rc.br.tickcam;
    /* OPENPETE_ALPHA_LOG: the bracket verdict of each present. */
    if (getenv("OPENPETE_ALPHA_LOG"))
        fprintf(stderr, "[alpha] vbl=%u key=%u paired=%d qa=%.4f alpha=%.4f bracketed=%d cut=%d hold=%d\n",
                openpete_frame, openpete_feed_key_now(), paired, qa, alpha, bracketed, cam_cut, hold);
    if (cam_dbg_on()) cam_dbg_present(cam, &rc, paired, prev, curr);
    if (cam)
        cut_census_camera((const int32_t*)(cam + CAM_OFF_POS),
                          (const int16_t*)(cam + CAM_OFF_SKY), alpha, cam_cut);
    openpete_native_scene_begin();
    RX_CENSUS_BEGIN();
    { OP_PROF_ACCUM_SCOPE("rex.cyclorama");
      cyclorama_reextract(cpu, bracketed ? alpha : 0.0, bracketed);   /* sky dome + background */ }
    RX_CENSUS_STEP("cyclorama");
    /* A held cut shows tick N-1's portal set: terrain reads its opening
     * planes and the portal feeder draws its previews. */
    portal_s23_reextract_enter(hold);
    { OP_PROF_ACCUM_SCOPE("rex.terrain");
      terrain_reextract(cpu, bracketed ? alpha : 0.0, bracketed, 0); }
    RX_CENSUS_STEP("terrain");
    { OP_PROF_ACCUM_SCOPE("rex.portal");
      portal_s23_reextract(cpu, bracketed ? alpha : 0.0, bracketed);   /* homeworld portal previews */ }
    portal_s23_reextract_leave();
    RX_CENSUS_STEP("portal");
    /* The two model kinds are queued and replayed apart: the animated queue is
     * a staged view of the tick's draw scratch, the simple models' queue a host
     * copy. A state whose tick ran one draw and not the other replays the one
     * it ran, and the passes inside skip on their own producer's freshness. */
    { OP_PROF_ACCUM_SCOPE("rex.moby");
    const int moby_staged = stage_moby_records();
    if (moby_staged || moby_native_simple_fresh()) {
        /* The cull stores each animated record already composed against the
         * tick's camera, in view space; hand the replay that camera so it can
         * re-base onto the staged one. Cleared after the pass, so the canonical
         * inline render replays the records untouched. */
        moby_native_set_tick_camera(tickcam ? (const int16_t*)(tickcam + CAM_OFF_SKY) : NULL,
                                    tickcam ? (const int32_t*)(tickcam + CAM_OFF_POS) : NULL);
        /* A held cut shows tick N-1's camera for the whole interval: replay the
         * queue the cull built against that camera, so a moby only N-1 could
         * see stays on screen until the cut lands. */
        moby_native_replay_prev_queue(hold);
        /* Arm the per-object sub-tick clock for this feeder pass: it is the
         * timebase the keyframe crossfade reads, so without it every replayed
         * record holds the tick's mesh while its translation sweeps. Reset
         * after the pass so the canonical inline render stays byte-exact. */
        moby_native_set_rot_interp(1);
        moby_native_set_verdict_alpha(alpha, bracketed);
        moby_native_set_late(late);
        moby_reextract(cpu);        /* the 3D actors, after the terrain they stand on */
        moby_native_set_late(0);
        moby_native_set_verdict_alpha(1.0, 1);
        moby_native_set_rot_interp(0);
        moby_native_replay_prev_queue(0);
        moby_native_set_tick_camera(NULL, NULL);
    }
    }   /* rex.moby */
    RX_CENSUS_STEP("moby");
    /* The same hold for the player, the moby drop-shadows, the particles and
     * every producer after them: the set tick N-1 drew is the set its camera
     * sees. */
    player_records_set_hold(hold);
    if (player_ranges_stage(hold))
        { OP_PROF_ACCUM_SCOPE("rex.player");
          player_records_replay(cpu, bracketed, alpha);   /* every player call the tick drew */ }
    player_records_set_hold(0);
    RX_CENSUS_STEP("player");
    shadow_replay_prev_list(hold);
    { OP_PROF_ACCUM_SCOPE("rex.shadow");
      shadow_reextract(cpu);          /* drop-shadows under mobys */ }
    shadow_replay_prev_list(0);
    RX_CENSUS_STEP("shadow");
    particles_replay_prev(hold || late);
    { OP_PROF_ACCUM_SCOPE("rex.particles");
      particles_reextract(cpu, bracketed, alpha);   /* the particle primitives the tick captured */ }
    particles_replay_prev(0);
    RX_CENSUS_STEP("particles");
    /* The flame records are the player's, so the player hold picks them. */
    player_records_set_hold(hold);
    { OP_PROF_ACCUM_SCOPE("rex.flame");
      flame_records_present(cpu, bracketed, 0, bracketed ? alpha : 0.0, NULL); }
    player_records_set_hold(0);
    RX_CENSUS_STEP("flame");
    { OP_PROF_ACCUM_SCOPE("rex.tracers");
      tracers_reextract(cpu, bracketed, alpha, hold);   /* the tracer primitives the tick captured */ }
    RX_CENSUS_STEP("tracers");
    { OP_PROF_ACCUM_SCOPE("rex.glow");
      glow_reextract(cpu, bracketed, alpha, hold);      /* the glow fans the tick captured */ }
    RX_CENSUS_STEP("glow");
    { OP_PROF_ACCUM_SCOPE("rex.glint");
      glint_reextract(cpu, bracketed, alpha, hold);     /* the gem-glint stars the tick captured */ }
    RX_CENSUS_STEP("glint");
    { OP_PROF_ACCUM_SCOPE("rex.zap");
      zap_reextract(cpu, bracketed, alpha, hold);       /* Zoe's zap, the ribbons the tick captured */ }
    RX_CENSUS_STEP("zap");
    /* The HUD, dialog boxes, text and screen fades are produced during the
     * game's own render and cannot be re-derived here, so the canonical present
     * banks them and every re-extract re-applies that carry on top of the
     * re-derived 3D scene (fade interpolated by the alpha, held panels
     * re-emitted as captured). Without it a sub-tick present draws the world
     * with no 2D layer at all. */
    { OP_PROF_ACCUM_SCOPE("rex.overlay");
      openpete_native_scene_apply_overlay(bracketed ? alpha : 0.0); }
    RX_CENSUS_END(bracketed, alpha);
    s_fpos_active = 0;
    cut_obj_present_end();
}

/** @brief Install the sub-tick re-extract body with the native renderer (game_reextract_init). */
static void orch_reextract_init(void) {
    game_reextract_view_install(game_reextract_body);
}

/* The orchestrator spyro2_game_runtime and spyro3_game_runtime carry
 * (games/spyro-{2,3}/game_extras_impl.c); the slots left NULL take the inert
 * entries. */
const game_orchestrator_t spyro23_orchestrator = {
    .reextract_init  = orch_reextract_init,
    .interp_pos      = orch_interp_pos,
    .epoch_commit    = orch_epoch_commit,
    .epoch_supersede = orch_epoch_supersede,
    .epoch_stamp     = orch_epoch_stamp,
    .epoch_fresh     = orch_epoch_fresh,
    .mobyreg_fresh   = orch_mobyreg_fresh,
    .capture_range   = orch_capture_range,
    .capture_recs    = orch_capture_recs,
};
