/* moby_snap.h - contact planes of pulled mobys against the drawn terrain
 * (the CPU half of the sunk-pickup depth snap, psx_moby_snap.glsl).
 *
 * The game inserts every face of a moby m_DepthOffset ordering-table slots
 * nearer than its average depth (r_moby.s / the moby draw's OT insert), so a
 * pickup set into the ground paints whole over that ground. The moby walk
 * hands each pulled moby's triangles here as it emits them; once the
 * terrain build that feeds the same scene has run, the triangles are tested
 * against the drawn opaque terrain they reach, and each moby's contact
 * planes go to the scene's snap table (openpete_native_snap_add). Either
 * feeder may run first in a scene: the second one resolves. */
#ifndef OPENPETE_COMMON_RENDERER_MOBY_SNAP_H
#define OPENPETE_COMMON_RENDERER_MOBY_SNAP_H
#include <stdint.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */
#ifdef __cplusplus
extern "C" {
#endif
struct CPUState;

/** @brief Queue one pulled moby of the scene now accumulating for the contact test.
 *
 * Starts a new queue when the scene changed since the last call.
 * @param mid The moby identity (openpete_scene_tri.mid), nonzero.
 * @param mordinal The moby's present-local ordinal, 0 when no face is ranked.
 * @param dofs The authored depth offset (m_DepthOffset), > 0.
 * @param off_m Key depth less raster depth of its faces, world units.
 * @param v Its triangles' corners: PSX screen x, y and key view-Z
 *        (full-world), three corners per triangle.
 * @param n Triangle count. */
void moby_snap_queue(uint32_t mid, int mordinal, int dofs, float off_m,
                     const float (*v)[3][3], int n);
/** @brief The lean queue's mode (OPENPETE_SNAP_QUEUE_LEAN, OPENPETE_SNAP_QUEUE_LEAN_CHECK), read once.
 *
 * On, a queued moby's bounds are computed when its chunk resolves and the
 * collect gathers its corners into moby_snap_queue_room; under the check
 * both are also done the old way and compared.
 * @return 0 off, 1 on (the default), 2 on and checked. */
int moby_snap_queue_lean(void);
/** @brief Room for n triangles at the end of the queue of the scene now accumulating, to gather one moby's corners into before moby_snap_queue takes them in place.
 *
 * Starts a new queue when the scene changed. The room holds until the next
 * call into the queue; moby_snap_queue given this pointer copies nothing.
 * @param n The most triangles the caller writes.
 * @return The room, or NULL when the lean queue is off or on an allocation failure. */
float (*moby_snap_queue_room(int n))[3][3];
/** @brief Whether the carry verify's corner probe is on (OPENPETE_MOBY_SNAP_CARRY_VERIFY_CMOV=1), read once.
 * @return 1 when on, else 0. */
int moby_snap_probe_on(void);
/** @brief Attach the probe's tags to the moby moby_snap_queue last queued: its triangles' face keys and its record path.
 *
 * Ignored unless the probe is on and the last queued moby is `mid` with `n` triangles.
 * @param mid The moby identity.
 * @param fk The triangles' face keys, in queue order.
 * @param n Their count.
 * @param path The record path the walk took for the moby (1 cull record, 2 host-built record, 3 shaded, 0 other).
 * @param kr Its first eligible corner's key depth over its perspective W (kvz / tvz). */
void moby_snap_queue_probe(uint32_t mid, const uint32_t* fk, int n, int path, float kr);
/** @brief Note one walk pass's ranked mobys, ordinal base + k + 1 for mids[k], for the ordinal-collision census.
 * @param mids The pass's ranked moby identities in ordinal order.
 * @param n Their count.
 * @param base The ordinals the scene's earlier passes used. */
void moby_snap_note_ranked(const uint32_t* mids, int n, int base);
/** @brief Mark the resolves that follow as a sub-tick re-extract's at a phase, or as canonical.
 *
 * A canonical resolve stores its planes for the snap carry; a re-extract's
 * resolve carries the planes of the endpoint the phase selects
 * (OPENPETE_MOBY_SNAP_CARRY) and runs the carry verify
 * (OPENPETE_MOBY_SNAP_CARRY_VERIFY). Leaving a re-extract (rex 0 after a
 * nonzero call) resolves the queue of all its moby passes at once
 * (OPENPETE_SNAP_REX_MERGE, default on) and emits the present's snap counts.
 * @param rex Nonzero for a sub-tick re-extract.
 * @param alpha The present's phase, 0 at tick T-1 and 1 at tick T; read when rex is nonzero. */
void moby_snap_present(int rex, double alpha) OP_REQUIRES(sim);
/** @brief End a moby walk pass: resolve the queued mobys when the terrain of this scene has been built, except in a sub-tick re-extract with the merge on, where the queue waits for the re-extract's end (moby_snap_present). */
void moby_snap_moby_fed(void) OP_REQUIRES(sim);
/** @brief Resolve the queued mobys after the terrain build that fed this scene.
 * @param cpu The guest CPU state, unused; the terrain feeder's call site signature. */
void moby_snap_terrain_fed(struct CPUState* cpu) OP_REQUIRES(sim);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_MOBY_SNAP_H */
