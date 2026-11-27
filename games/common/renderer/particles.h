/* particles.h - the particle feeder's sub-tick entry.
 *
 * The feeder's geometry is the primitives the guest routine wrote, which exist
 * only during the canonical tick's draw, so the sub-tick re-extract replays the
 * set captured then rather than re-deriving it. */
#ifndef OPENPETE_RENDERER_PARTICLES_H
#define OPENPETE_RENDERER_PARTICLES_H

#include "psx_runtime_internal.h"
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/** @brief Re-feeds the particle primitives the canonical tick captured, rebased onto the pose this present shows.
 *
 * The primitives are the guest's own output and are gone once the polybuf is
 * reused, so the replay re-feeds the captured set: each record point's screen
 * displacement carries its primitive's corners and its depth. The replay
 * recomputes the rebase only while the pass has a camera pair; across a cut
 * or an unpaired present it skips the rebase and feeds the set with the
 * displacements the last bracketed call since the capture left in place,
 * or none when no bracketed call ran since the capture. At the end of the
 * tick's interval every term of the rebase
 * is zero and the replay reproduces the canonical present exactly. Does
 * nothing until the particle path is ready for re-extract.
 * @param cpu Reads the staged camera, the pool snapshots and the sin/cos table.
 * @param bracketed The pass's camera-pair verdict.
 * @param alpha The pass's phase between the two ticks it presents between,
 *        passed as the pass holds it and read only when `bracketed` is set.
 *        The staged camera was built at this alpha, so the pool positions the
 *        rebase projects under it are phased onto the same instant. */
void particles_reextract(CPUState* cpu, int bracketed, double alpha) OP_REQUIRES(sim);

/** @brief Make the particle re-extract show tick N-1's captured set, for a held camera cut or a present before alpha 1, or restore the current one.
 *
 * A held cut shows tick N-1's camera, and the primitives the draw emitted that
 * tick are the ones visible under it; a present before alpha 1 shows tick
 * N-1's set (the late switch), each record lerped toward its tick N state,
 * held at its N-1 state when the slot was freed or reused at tick N, and a
 * record new at tick N absent. The set is chosen by cut_held_set_pick:
 * the previous set swapped in, the current set when tick N captured nothing,
 * or no set when tick N-1 captured nothing; whether tick N drew the path does
 * not gate the held feed. While on, each record's offsets are measured from
 * the pool state that draw read (the pair's prev for a slot that rolled after
 * tick N's draw stamp). Call on before particles_reextract and off after it.
 * @param on Nonzero to choose the held set, zero to restore.
 * @return 1 while a held set is shown, else 0. */
int particles_replay_prev(int on);

/** @brief The pool slot index of the particle record at `rec`.
 *
 * Indexes from the pool base the slot allocator hook last read.
 * @param rec The record's guest address.
 * @return The slot index, or -1 when no allocation has been seen, the address
 *         is not a record boundary of that pool, or the index lies beyond the
 *         slots tracked. */
int particles_slot(uint32_t rec);

/** @brief The serial of the allocation that last handed out the slot of the particle record at `rec`.
 *
 * The slot allocator hook gives every allocation a serial naming its tick and
 * its ordinal in that tick (alloc_serial_next), so a slot holds a different
 * serial after every allocation within 2^20 ticks and two slots never hold
 * the same nonzero serial. A record whose serial is unchanged between two
 * captures is one continuing particle.
 * @param rec The record's guest address.
 * @return The serial, or 0 when no allocation of the slot has been seen. */
uint32_t particles_slot_serial(uint32_t rec);

#endif /* OPENPETE_RENDERER_PARTICLES_H */
