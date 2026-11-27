// games/common/renderer/glint.h - the gem-glint feeder's sub-tick entry point.
#ifndef OPENPETE_GLINT_H
#define OPENPETE_GLINT_H

#include "psx_runtime_internal.h"
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/** @brief Re-feed the captured glint primitives, rebased onto the pose this present shows.
 *
 * The primitives are the guest's own output and are gone once the polybuf is
 * reused, so the replay re-feeds the captured set. When bracketed, each
 * primitive is first rebased: its owning record's screen displacement carries
 * the corners and the depth, the change in spin phase rotates the corners
 * about that point and the change in extent scales them about it. When not
 * bracketed the set is fed with whatever rebase state it holds. At the end of
 * the tick's interval every term of the rebase is zero and the replay
 * reproduces the canonical present. A held camera cut shows tick N-1's set
 * (cut_held_sets_enter) as that tick drew it, unrebased, since the staged
 * camera is then tick N-1's. Does nothing until the path is ready for a
 * re-extract (hook_reextract_ready_held for RP_SPARKLE). Writes no PSX RAM.
 * @param cpu The guest CPU state, read only.
 * @param bracketed Nonzero to rebase the primitives at alpha before feeding.
 * @param alpha The present's position in the tick interval, 1 at its end.
 * @param hold Nonzero on a held camera cut. */
void glint_reextract(CPUState* cpu, int bracketed, double alpha, int hold) OP_REQUIRES(sim);

#endif
