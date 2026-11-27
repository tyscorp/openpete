// games/common/renderer/zap.h, Zoe's zap feeder's sub-tick entry point.
#ifndef OPENPETE_ZAP_H
#define OPENPETE_ZAP_H

#include "psx_runtime_internal.h"
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/** @brief Re-feeds the tick's held ribbon primitives for a sub-tick present, rebased onto the present's pose.
 *
 * A no-op unless the tracer path's re-extract is ready. When the tick's
 * zap call ran under the hook bracket the ribbon ends are re-projected
 * and every corner is moved by the blended displacement; otherwise the
 * captured corners and paired depths feed verbatim. The primitives are
 * the guest's output held from the canonical tick; nothing is read from
 * the polybuf and no PSX RAM is written. A held camera cut shows tick N-1's
 * set (cut_held_sets_enter) verbatim, since the staged camera is then tick
 * N-1's.
 * @param cpu The guest CPU state.
 * @param bracketed Nonzero when the tick's call ran under the bracket.
 * @param alpha The present's sub-tick position, used when bracketed.
 * @param hold Nonzero on a held camera cut. */
void zap_reextract(CPUState* cpu, int bracketed, double alpha, int hold) OP_REQUIRES(sim);

#endif
