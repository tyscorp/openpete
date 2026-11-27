/* glow.h : the glow-ring feeder's export. */
#pragma once
#include "psx_runtime_internal.h"
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/** @brief Replay the tick's captured glow fans on a sub-tick present.
 *
 * The primitives are the guest's own output and are gone once the polybuf is
 * reused, so the replay re-feeds the set the canonical tick captured, at the
 * depths paired then, rebased onto the screen position the staged camera and
 * the record's own motion put them at. At alpha 1 the rebase is zero and the
 * replay reproduces the canonical present exactly. A held camera cut shows
 * tick N-1's set (cut_held_sets_enter) as that tick drew it, unrebased, since
 * the staged camera is then tick N-1's. Does nothing unless
 * hook_reextract_ready_held(RP_GLOW, ...) holds.
 * @param cpu The guest CPU state, read only.
 * @param bracketed Nonzero when the present lies inside a paired tick
 *        interval; zero skips the rebase and re-feeds at the displacements
 *        last computed.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @param hold Nonzero on a held camera cut. */
void glow_reextract(CPUState* cpu, int bracketed, double alpha, int hold) OP_REQUIRES(sim);
