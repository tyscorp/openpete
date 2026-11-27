/* flame.h - the player's fire-breath ribbon producer (renderer/flame.c).
 *
 * The producer hooks the game's flame emitter (game_layout.flame.hook), runs
 * it once, and re-derives the same ribbon from gamestate in float for the
 * native scene. The sub-tick entries below let a game's re-extract replay one
 * ribbon per flame record at a present between two ticks. */
#ifndef OPENPETE_RENDERER_FLAME_H
#define OPENPETE_RENDERER_FLAME_H

#include <stdint.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/** @brief Set the sub-keyframe slide the next ribbon walks apply to each cross-section.
 *
 * The slide is the fractional part of the per-strand keyframe-counter
 * advance: each cross-section is lerped from its spine record toward the next
 * one by this fraction, so the ribbon animates between the static keyframes.
 * 0 restores exact keyframes, which is what the canonical inline render uses.
 * @param frac The fraction; clamped to [0, 1]. */
void flame_native_set_subframe(double frac);

/** @brief Re-derive one flame record's ribbon into the native scene for a sub-tick present.
 *
 * Extract-only: the game's emitter does not run. Returns without effect when
 * the game names no flame state, the native scene is off, the flame render
 * path is excluded or was not drawn by the canonical tick (on a held cut, when
 * the open records are tick N-1's, the canonical-tick test is dropped), or the staged
 * m_IsFlameActive word is 0 (the emitter runs only while the player breathes
 * fire, so an unconditional feed would draw a stale ribbon). The ribbon
 * attaches to the staged pose and projects through the interpolated camera.
 * @param cpu The guest CPU state, read only.
 * @param slot The model record the ribbon renders under. */
void flame_reextract(struct CPUState* cpu, int slot) OP_REQUIRES(sim);

/** @brief Interpolate one prev/curr pair of flame-state bytes into a staged copy.
 *
 * The origin and the attach matrix are lerped at `alpha`. Each of the 8
 * per-strand keyframe counters whose tick delta is within (-8, 8) is written
 * as the floor of the interpolated counter (at least 0); a larger delta is
 * the counter's wrap on reseed and keeps curr. The fractional part of the
 * first advancing strand is written to *subframe while *subframe is 0; every
 * strand advances by the same delta, so the slide is shared. When the origin
 * moved more than 5000 units (the first active tick, whose prev pose predates
 * activation) nothing is written.
 * @param fl The staged flame-state bytes, holding curr on entry.
 * @param pfl The previous tick's flame-state bytes.
 * @param cfl The current tick's flame-state bytes.
 * @param alpha The sub-tick fraction.
 * @param subframe Receives the sub-keyframe slide; the caller initializes it
 *        to 0.
 * @return 1 when it interpolated, 0 when it snapped. */
int flame_bracket(uint8_t* fl, const uint8_t* pfl, const uint8_t* cfl,
                  double alpha, double* subframe);

/** @brief Run every flame record of this present at the producer's place in the scene order.
 *
 * The loop runs after the particles. Each drawn flame record copies its own
 * flame-state bytes (the led tick's bytes under --runahead) over the staged
 * span, brackets origin, attach matrix and strand counters against the
 * previous list's flame record of the paired model when that model lerps,
 * and runs flame_reextract under the record's model slot with the record's
 * sub-keyframe slide. The walk reads no avatar field, so the model bytes
 * are not restaged. The staged span is a host view copy (the guest span itself when staging
 * is refused): the ambient bytes
 * are restored and the slide reset to 0 at the end.
 * @param cpu     guest state
 * @param paired  1 when the interp ring paired this present
 * @param cam_cut 1 when the camera cut between the paired ticks
 * @param alpha   the present's sub-tick alpha
 * @param fl      the caller's ambient staged copy of the flame span, or NULL
 *                to let the loop stage the span itself */
void flame_records_present(struct CPUState* cpu, int paired, int cam_cut,
                           double alpha, uint8_t* fl) OP_REQUIRES(sim);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_RENDERER_FLAME_H */
