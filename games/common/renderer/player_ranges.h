/* games/common/renderer/player_ranges.h - the player draw's part-transform
 * ranges, copied at the player hook and staged back for a sub-tick present.
 *
 * The player draw's part-transform records live in the per-tick draw scratch,
 * at a fixed offset the player hook names, and the passes after it reuse those
 * bytes before any sub-tick present runs. They are copied at the hook and
 * staged over their own address for each sub-tick present, so the player
 * replay decodes the records the tick drew instead of whatever overwrote them.
 *
 * A slot per range the hook names in a tick, up to two, so no named range
 * is left unstaged. An unstaged span is not merely stale: the replay's
 * two-pass decode writes each rendered pose into the block it reads, and
 * outside the sub-tick view that host pointer is live PSX RAM, so the present
 * would edit gameplay memory. Slots are keyed by the scratch offset the hook named.
 *
 * Two generations: the slots as of the last canonical epoch that captured, and
 * as of the capturing epoch before it, so a held camera cut stages tick N-1's
 * ranges (cut_held_set_pick). */
#ifndef OPENPETE_GAME_PLAYER_RANGES_H
#define OPENPETE_GAME_PLAYER_RANGES_H

#include "psx_runtime_internal.h"
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Copies one sub-range of the player's draw scratch into its slot of the current generation.
 *
 * The first capture of a new canonical epoch retires the current generation,
 * whole, to the previous one; a slot the new epoch does not capture again
 * keeps its older epoch and fails the generation's stage. The range sits at
 * the polybuf end word minus game_layout.player.part_base_polybuf_off plus
 * @p off. A length over the slot's copy, an unreadable span, or no free slot
 * leaves the slot empty.
 * @param cpu Reads the polybuf end word.
 * @param off The range's offset into the draw scratch, the slot key.
 * @param len The range's length in bytes. */
void player_ranges_capture(CPUState* cpu, unsigned off, unsigned len) OP_REQUIRES(sim);

/** @brief Stages a generation's captured ranges over their own addresses for the present in flight.
 *
 * Outside a hold the generation of the last committed canonical epoch; on a
 * held cut tick N-1's generation (cut_held_set_pick), or none when tick N-1
 * captured none. A slot whose epoch is not the generation's, or whose stage
 * fails, makes the call return 0 at once, so the caller skips the replay; a
 * range staged before that failure stays staged for the present.
 * @param hold Nonzero on a held camera cut.
 * @return The number of ranges staged, or 0 when none is captured or any
 *         range is stale or unstaged. */
int player_ranges_stage(int hold);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_PLAYER_RANGES_H */
