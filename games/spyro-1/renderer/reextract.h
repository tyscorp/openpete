/* games/spyro-1/renderer/reextract.h - Spyro 1's sub-tick re-feed
 * orchestrator.
 *
 * On each intermediate (sub-tick) present the core spine
 * (openpete_native_register_reextract) calls our orchestrator, which:
 *   1. queries the interp ring for prev/curr camera + alpha,
 *   2. brackets g_Camera (save -> write the interpolated camera -> restore),
 *      entirely on the host fiber so the next sim tick reads canonical RAM,
 *   3. rebuilds the native scene by calling each feeder's extract-only
 *      re-extract entry (NO game render call, NO verdict writes).
 *
 * Feeders that read g_Camera directly (terrain, ...) interpolate for free via
 * the bracket; feeders that bake the camera get converted to re-derive from
 * g_Camera incrementally. Determinism is by the restore discipline + the
 * oracle gate.
 */

#ifndef SPYRO_RENDERER_REEXTRACT_H
#define SPYRO_RENDERER_REEXTRACT_H

#include <stdint.h>   /* uint32_t (canonical-render epoch API) */

/* The interface the shared renderer TUs call into; Spyro 1 defines it here
 * (reextract.c) and in cyclorama.c. */
#include "renderer/orchestrator.h"
#include "renderer/terrain.h"        /* the shared terrain feeder's entries */
#include "renderer/cyclorama.h"      /* cyclorama_reextract, cyclorama_native_extract_faded */
#include "renderer/moby.h"          /* the shared moby feeder's entries */
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Per-feeder re-extract entries (defined in each feeder TU). Extract-only:
 * they reproduce the feeder's scene contribution from current gamestate +
 * the (interpolated) g_Camera the orchestrator has written, without calling
 * the game render function or touching gameplay-readable RAM. */
/* player_native_set_*: defined in games/common/renderer/player.c, documented in games/common/renderer/player.h. */
void player_native_set_anim_interp(double alpha, int on);
void player_native_set_anim_pair(const uint8_t* prev_seg, const uint8_t* curr_seg);
void player_native_set_player_slot(int k);
void player_native_set_flat_key(uint32_t key);

void player_native_set_rot_interp(const int* active, const double* ang);

/** @brief Sub-tick rotation for the dragon-rescue star (D_80076248).
 *
 * The rotation is the record's +0x28 euler bytes lerped at float precision,
 * {Z, X, Y} byte angles, which the star feeder composes through the resolved camera
 * matrix in place of the record's bytes. NULL resets, so a canonical present
 * composes from the bytes. Defined in renderer/particle.c.
 * @param ang_zxy three byte angles {Z, X, Y}, or NULL. */
void dragonstar_native_set_rot_interp(const double* ang_zxy);

/** @brief Portal previews of a sub-tick present.
 *
 * Replays each leaf capture of the last canonical RenderPortals through the
 * canonical feeders (feed_portal_quad, feed_portal_dome). The dome rotation
 * matrix is captured prev+curr per canonical tick and lerped by the spine's
 * sub-tick `alpha` (componentwise, like the camera matrix), snapping to curr
 * when a component moved more than 1200 in the tick; a dome the canonical feed
 * drew with the derived matrix re-derives it instead. Each capture's clip rect
 * is rebuilt under the present's camera by its rect rule. The clip rect
 * (D_8007AA00) goes through the sub-tick view stage or is restored, and the
 * GTE rotation registers are restored. Draws nothing when the native scene is
 * off, nothing was captured, or RP_PORTAL was not drawn on the canonical
 * present (RenderPortals stops during a level load and leaves a stale capture).
 * On a held camera cut the replay shows tick N-1's capture set and dome matrix
 * (cut_held_sets_enter), no set when RenderPortals captured none in that
 * epoch. The set stays selected for the terrain feeder's plane provider until
 * portal_reextract_leave. Each replayed capture, with the triangles it
 * emitted, and each refused replay is recorded in the bug-report sub-tick
 * ring (openpete_bugrec_subtick_portal) and, under
 * OPENPETE_PORTAL_PRESENT_DBG=1, printed as a [portal-present] line.
 * @param cpu       guest state, the sub-tick view staged
 * @param alpha     sub-tick fraction between the previous and current tick
 * @param interp_on 1 = camera bracketed, else snap to curr
 * @param hold      nonzero on a held camera cut */
void portal_reextract(struct CPUState* cpu, double alpha, int interp_on, int hold) OP_REQUIRES(sim);

/** @brief Restore the current capture set after a re-extract's terrain feeder has read it.
 *
 * Pairs every portal_reextract; canonical renders see no held set. */
void portal_reextract_leave(void);
/* terrain_reextract, terrain_native_topdown_pose, terrain_native_sweep_pose:
 * renderer/terrain.h (the shared feeder). */
void shadow_reextract(struct CPUState* cpu) OP_REQUIRES(sim);
/** @brief Feeds the Spyro silhouette shadow of one shadow record for a sub-tick present.
 *
 * The record loop of the sub-tick re-extract calls it once per record with
 * that record's g_Spyro fields and ground anchor staged. Behind the same
 * gate as the moby shadows (hook_reextract_ready for RP_SHADOW); draws
 * nothing when the gate is closed, the player shadow feed is disabled, or
 * Spyro is over water. Extract-only: no PSX RAM store.
 * @param cpu The guest CPU state, the record's fields staged.
 * @param slot The model record index the shadow renders under; negative
 *        reads as 0. */
void shadow_spyro_reextract(struct CPUState* cpu, int slot) OP_REQUIRES(sim);
void player_reextract(struct CPUState* cpu) OP_REQUIRES(sim);
/** @brief Replays the particle, glow, sparkle and star feeds for a sub-tick present.
 *
 * Returns when the native scene is off or no feeder's path is enabled and
 * canon-drawn. Each pool feeds only when its own renderer drew on the
 * canonical tick; the star follows the particle path. Extract-only: the
 * .s does not run and no verdict byte is written. Latches s_phase_now to the
 * present clock around the pool feeds so the channel interpolation is on,
 * and clears it afterwards so the inline hook feeds stay integer-exact. On a
 * held camera cut the glow and sparkle feeds re-derive tick N-1's pool copy
 * (cut_held_sets_enter), staged over the pool, with the interpolation off.
 * @param cpu The guest CPU state.
 * @param hold Nonzero on a held camera cut. */
void particle_reextract(struct CPUState* cpu, int hold) OP_REQUIRES(sim);
/** @brief Collect the owner mobys of the live glow records into out.
 *
 * A live record (D_80078800[16], stride 0x24) has a positive ring count at
 * +0x00 and a MobyPos pointer at +0x08 into its owner's m_Position (+0x0C).
 * The owner is kept when its record is mapped, alive (m_State below 0x80)
 * and of a class with a model row. Duplicates are dropped.
 * @param out Receives up to 16 owner moby addresses.
 * @return The number written. */
int particle_glow_owner_mobys(uint32_t out[16]);
/** @brief Sub-tick re-extract that replays the tracer snapshot against the current camera.
 *
 * Does nothing unless the tracer path was drawn on the canonical tick, so a
 * tick with no tracer does not replay a stale ribbon.
 * @param cpu The guest CPU state. */
void tracer_reextract(struct CPUState* cpu) OP_REQUIRES(sim);
/** @brief Re-derives and feeds the level-transition and balloonist cross-fade dome and backdrop on a sub-tick present.
 *
 * Re-runs the extraction rather than replaying the canonical triangles, so
 * the dome turns with the interpolated g_Camera. Gated on the native scene
 * and on render_path_canon_drawn(RP_TRANSITION), latched per canonical
 * present from the func_8004F000 hook's recorded polybuf range, so the dome
 * stops once the transition stops emitting. OPENPETE_LOAD_CENSUS logs the
 * replay count every 100 openpete_frame values. A held camera cut feeds tick
 * N-1's dome (cut_held_sets_enter) as that tick extracted it, under the
 * staged camera of that tick, whatever tick N drew.
 * @param cpu The guest CPU state with the sub-tick view staged.
 * @param hold Nonzero on a held camera cut. */
void transition_reextract(struct CPUState* cpu, int hold) OP_REQUIRES(sim);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_RENDERER_REEXTRACT_H */
