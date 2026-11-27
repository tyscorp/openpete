/* renderer/moby.h - the shared moby (3D actor) feeder's entries.
 *
 * The walk lives in games/common/renderer/moby.c and reads every engine
 * address from game_layout.moby; the per-game stream formats and the data a
 * game keeps outside those addresses come through moby_game
 * (renderer/moby_game.h). A game reaches the feeder by listing moby_module in
 * its feature list; a game whose layout names no draw hook registers nothing
 * and the walk never runs. */
#ifndef OPENPETE_COMMON_RENDERER_MOBY_H
#define OPENPETE_COMMON_RENDERER_MOBY_H

#include <stdint.h>
#include "renderer/moby_game.h"   /* moby_wface_t: the shared face record */
#include "renderer/moby_rank.h"   /* paint-key records and vertices */
#include "renderer/native_gte.h"  /* ngte_euler_sc: the composed rotation's sin/cos */
#include "openpete_time.h"         /* op_time_t */
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/** @brief Sub-tick moby rotation interpolation toggle.
 *
 * When on, the moby euler composer renders each moby's rotation at float
 * precision - shortest-arc interpolating the byte euler (256/circle) between
 * the moby's prev/curr interp-objs snapshots at its per-object alpha, with the
 * sin/cos sampled by linear interpolation of the in-RAM table - instead of the
 * byte-quantized angle. Removes the 1.4°/step stair-stepping on slowly-rotating
 * mobys (gems, idle dragons, HUD icons). Set by the orchestrator around the
 * feeder re-extract; reset (off) so the canonical inline render stays
 * byte-exact. Turning it off after a sub-tick present emits that present's
 * OPENPETE_MOBY_CENSUS `pose_snap` line.
 * @param on 1 to open a sub-tick present's feeder pass, 0 to close it */
void moby_native_set_rot_interp(int on);

/** @brief Set the interpolation alpha of the present being re-extracted and whether the camera bracket ran.
 *
 * Alpha 0 shows the previous canonical tick, 1 the latest; an unbracketed
 * present shows the latest and reads as alpha 1. The sub-tick carry of the
 * same-moby paint verdict applies the previous tick's verdict below 0.5 and
 * the latest tick's from 0.5. Set by the orchestrator before moby_reextract
 * and reset to (1, 1) after.
 * @param alpha The present's interpolation alpha.
 * @param bracketed 1 when the camera bracket ran, else 0. */
void moby_native_set_verdict_alpha(double alpha, int bracketed);

/** @brief Install the camera the cull composed this tick's records against, or clear it with NULL.
 *
 * For a game whose cull stores view-space records (moby_game.host_reveal ==
 * 0): the interp ring's curr snapshot, nine int16 matrix entries in GTE row
 * order (4096 = 1.0) and the three camera position words. With it the
 * sub-tick record replay re-bases each record onto the camera the present
 * stages, so an animated moby follows the interpolated view like every other
 * feeder instead of holding its tick pose in screen space. A NULL argument
 * clears it and the replay leaves every record as the cull wrote it. The
 * orchestrator sets it around the feeder re-extract; a game that rebuilds
 * records host-side does not read it.
 * @param mat The matrix, or NULL.
 * @param pos The camera position, or NULL. */
void moby_native_set_tick_camera(const int16_t mat[9], const int32_t pos[3]);

/** @brief Make the sub-tick record replay walk the previous canonical epoch's record queue, or restore the current one.
 *
 * For a held camera cut: the present shows tick N-1's camera, and the queue
 * the cull built against that camera is the set it can see. On: the previous
 * epoch's record copy, its base and its append set swap in for the current
 * ones, the record re-base composes against the camera that cull used (the
 * installed tick camera, which the simple models' replay reads, is left
 * as it is), and the replay takes each object's state as of that cull as the
 * record's reference (rotation, own-motion offset and keyframe pair). Off:
 * everything swaps back. Refused when the previous copy is not from the epoch
 * immediately before a fresh current one, or carries no camera. Call off after
 * moby_reextract.
 * @param on Nonzero to swap the previous queue in, zero to restore.
 * @return 1 while the previous queue is swapped in, else 0. */
int moby_native_replay_prev_queue(int on);

/** @brief Make the sub-tick replay show tick N-1's record set (the late switch), or tick N's.
 *
 * On a present before alpha 1: a record whose moby first appeared in tick N's
 * object capture is left out (moby_native_late_spawned), and the previous
 * queue's records whose moby that capture evicted are replayed at the state
 * tick N-1 drew (moby_native_late_ghost_admit). Set by the orchestrator around
 * moby_reextract; a held cut's previous-queue replay already shows tick N-1's
 * set and ignores it.
 * @param on Nonzero on a present before alpha 1 with the switch enabled. */
void moby_native_set_late(int on);

/** @brief Whether the present in flight shows tick N-1's record set (moby_native_set_late, outside a held cut).
 *
 * @return 1 when active, else 0. */
int moby_native_late_active(void);

/** @brief Whether a late present leaves a queued record out because its moby first appeared in tick N's capture; counts the deferral.
 *
 * @param moby The record's moby.
 * @return 1 when the record is left out, else 0. */
int moby_native_late_spawned(uint32_t moby);

/** @brief Whether a late present replays a previous-queue record whose moby tick N's capture evicted, staging the moby's bytes at that state.
 *
 * The moby's span is staged in the sub-tick view and overwritten with the
 * evicted entry's curr (its state at tick N-1) until moby_native_late_unstage;
 * a moby the table still tracks (unless its slot took a new record in tick N,
 * which tick N spawned under the evicted key), one with no evicted entry, or a
 * span the view refuses is not replayed.
 * @param moby The record's moby.
 * @return 1 when the record replays, else 0. */
int moby_native_late_ghost_admit(uint32_t moby);

/** @brief Restore every moby span moby_native_late_ghost_admit staged to the bytes it held before. */
void moby_native_late_unstage(void);

/** @brief Print one replayed record's drawn state under OPENPETE_LATE_TRACE=<lo>:<hi> (tests/harness/check_late_switch_subtick.sh).
 *
 * `[late-moby vbl=<v> leg=inter a=<alpha> src=curr|ghost|held pass=<pass>]
 * m=<moby>:<serial> cls=<class> n=<verts> c=(<x>,<y>) w=(<x>,<y>,<z>)` when the feed
 * key lies in the window: the record's source, the slot allocation serial of
 * the state drawn (0 when the blob carries none), its projected vertex count and
 * centroid, and the world position it is drawn at (the position words the view
 * resolves plus the replay's own-motion offset).
 * @param cpu The guest CPU state, read only.
 * @param moby The record's moby.
 * @param v Its projected vertices, screen X and Y.
 * @param n Their count; 0 prints nothing.
 * @param pass The pass's label.
 * @param ghost Nonzero for a despawned record a late present replays. */
void moby_native_late_trace(CPUState* cpu, uint32_t moby, const float (*v)[2], int n,
                            const char* pass, int ghost);

/** @brief The camera position the previous canonical epoch's cull composed its records against.
 *
 * @param out Receives the position when the previous queue copy is usable.
 * @return 1 when written, else 0. */
int moby_native_prev_cull_cam_pos(int32_t out[3]);

/** @brief The object-capture stamp current at the last canonical tick's moby cull, the draw stamp of that tick.
 *
 * An object whose curr is stamped after it rolled after that draw.
 * Not answered while a previous-queue replay is swapped in.
 * @param out Receives the stamp, when non-NULL.
 * @return 1 when the current queue copy is of the last canonical epoch, else 0. */
int moby_native_canon_ref_ns(op_time_t* out);

/** @brief The mobys of the previous canonical epoch's record queue, the set moby_native_replay_prev_queue would replay.
 *
 * Followed by the mobys of the simple models' queue copy of the same epoch,
 * when the game holds one (moby_game.simple_prev_mobys).
 * @param out Receives up to @p max moby addresses.
 * @param max Capacity of @p out.
 * @param ref_ns Receives the object-capture stamp current at the current
 *        queue's cull, when non-NULL: an object whose curr is stamped at or
 *        before it has not changed since the previous cull drew it.
 * @return The count written, or -1 when the previous queue is not usable. */
int moby_native_prev_queue_mobys(uint32_t* out, int max, op_time_t* ref_ns);

/** @brief Mark a re-extract whose game stages tick N-1's scratch for a held cut.
 *
 * The specular queue the sub-tick pass replays then follows the staged scratch
 * to tick N-1's capture.
 * @param on Nonzero on a held camera cut. */
void moby_native_held_scratch(int on);

/** @brief Whether a previous-queue replay (moby_native_replay_prev_queue) is in progress, with the epoch it replays and that cull's camera position.
 *
 * A second queue the same cull wrote (the simple models') replays its own
 * copy of that epoch under the same camera.
 * @param epoch Receives the replayed canonical epoch, when non-NULL.
 * @param cam_pos Receives the camera position that cull composed against,
 *        when non-NULL.
 * @return 1 while the previous queue is swapped in, else 0 (outputs untouched). */
int moby_native_prev_queue_replay(uint32_t* epoch, int32_t cam_pos[3]);

/** @brief The installed tick camera position, for a cull whose records bake the camera position into a stored per-record delta.
 *
 * The delta is re-based onto the staged camera by the difference between the
 * staged position and this one.
 * @param out Receives the three camera position words when a tick camera is
 *        installed; untouched otherwise.
 * @return 1 when a tick camera is installed; 0 otherwise, which is every
 *         canonical inline render. */
int moby_native_tick_camera_pos(int32_t out[3]);

/** @brief The horizontal widening of the pass view against the game's own 4:3 frame.
 *
 * The value is the view's F / s: Hor+ multiplies the frame's half-width by F, and a --fov scale s divides
 * the projection H by s, which widens it by the same ratio. 1.0 at 4:3 with
 * the game's FOV, so a test scaled by it is bit-identical there.
 *
 * A decode that repeats one of the draw's lateral cone tests scales that
 * test's slope by this, since the draw's slope is the 4:3 half-angle and the
 * frame on screen is wider. The vertical arms take moby_native_view_vwiden.
 * Valid from the pass view's build at the pass entry.
 * @return The widening factor. */
double moby_native_view_widen(void);

/** @brief The vertical widening of the pass view against the game's own 4:3 frame.
 *
 * The value is the view's 1 / s: Hor+ keeps the frame's half-height, and a
 * --fov scale s multiplies the projection H by s, so the half-height in the
 * game's screen coordinates is the band over s. Exactly 1.0 at the game's
 * FOV, so a vertical cone test scaled by it is the draw's own there. Valid
 * from the pass view's build at the pass entry.
 * @return The widening factor. */
double moby_native_view_vwiden(void);

/* ---- the present's own-motion terms, shared by both model kinds ---------
 * A cull record encodes the pose the moby held at the tick; the present shows
 * the moby part-way to it. These two carry the object's own movement over that
 * fraction, so a record placed from the tick's values steps and one that adds
 * them sweeps. Both read the per-object prev/curr snapshot pair and both return
 * the identity when there is none, so a record without a pair, a slot that
 * changed class between the two states and every canonical render stay exactly
 * as the cull wrote them.
 *
 * moby_record_world_offset fills out[] with the moby's world-position offset in
 * world units, (prev - curr) * (1 - t) at the present's phase t. Its axis order
 * is the moby's own position words; a caller in another convention maps the
 * components itself. Returns 0 with out[] zeroed when there is no usable pair,
 * including a stale one, where a multi-tick gap is a discontinuity rather than
 * motion. Returns 1 with a zero out[] for a moby static in the world. */
/** @brief The moby's world-position offset at the present's phase t, (prev - curr) * (1 - t) in world units.
 *
 * Its axis order is the moby's own position words; a caller in another
 * convention maps the components itself.
 * @param moby The Moby address.
 * @param out Receives the offset; zeroed when there is no usable pair.
 * @return 0 when there is no usable prev/curr pair, including a stale one,
 *         where a multi-tick gap is a discontinuity rather than motion; 1
 *         otherwise, with a zero out[] for a moby static in the world. */
int moby_record_world_offset(uint32_t moby, double out[3]);
/* The keyframe pair one moby's animation state names, by the cull's rule:
 * the AnimationHeaders of keyframes A and B, each keyframe's record word 0
 * (the stream word the vertex codecs open on) and the blend byte, 0 for
 * keyframe A alone. */
typedef struct {
    uint32_t header, header_b;
    uint32_t word_a, word_b;
    uint32_t blend;
} moby_keyframe_pair_t;

/** @brief The keyframe pair and blend byte a moby's five animation state bytes name, as the native moby walk derives them.
 *
 * The derivation is the walk's own (header A from anims[id], header B from
 * anims[nextId] when the mapped blend byte is non-zero and that slot holds a
 * header, else keyframe A with blend 0); the record word is read from the
 * keyframe record where the layout keeps the record's address.
 * @param cpu The guest CPU state, read only.
 * @param model The model-table entry.
 * @param st The five animation state bytes (id, nextId, frame, nextFrame,
 *        progress), live or from an interp snapshot.
 * @param out Receives the pair.
 * @return 0 when anims[id] is not a header or a record word does not map, else 1. */
int moby_keyframe_pair(struct CPUState* cpu, uint32_t model, const uint8_t st[5],
                       moby_keyframe_pair_t* out);

/** @brief The moby's displacement from the state its replayed record was composed from to the state this present shows.
 *
 * moby_record_world_offset, plus curr - prev for a record of a previous-queue
 * replay that was composed from the pair's prev (the object rolled after that
 * queue's cull); outside such a replay it equals moby_record_world_offset.
 * @param moby The Moby address.
 * @param out Receives the displacement in world units. */
void moby_replay_world_offset(uint32_t moby, double out[3]);
/** @brief The (prev - curr) * (1 - t) offset of one captured unsigned 16-bit moby field at the present's phase t.
 *
 * For a caller whose own convention is not the position words. The field is
 * read at byte offset off inside the object's captured record.
 * @param moby The Moby address.
 * @param off The field's byte offset inside the captured record.
 * @param out Receives the offset; zeroed when 0 is returned.
 * @return 0 when there is no usable prev/curr pair (as for
 *         moby_record_world_offset), when off lies past the captured blob, or
 *         when either state reads 0, which every such field uses as its
 *         absent value and which would otherwise sweep the object toward an
 *         origin it never held; 1 otherwise. */
int moby_record_halfword_offset(uint32_t moby, uint32_t off, double* out);
/** @brief The moby's three euler axes at the present's phase, as GTE sin/cos pairs.
 *
 * Each axis is interpolated by the shortest arc between the object's prev and
 * curr byte angles and sampled out of the in-RAM sine table by linear
 * interpolation, so a rotation that steps one byte per tick sweeps instead.
 * Z is byte +0 of the angle word, X +1 and Y +2; its high byte is an overlay
 * flag and no axis. A facing reversal (moby_mirror_hold) holds the previous
 * tick's angles until the hold expires, then takes this tick's.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param e Receives the sin/cos pairs; left untouched when 0 is returned.
 * @return 0 when the caller must use the byte-exact table lookup: rotation
 *         interp is off (every canonical render), there is no pair, the pair
 *         has no distinct prev, or the two states hold different classes,
 *         where a reused slot would lerp across two objects; 1 otherwise. */
int moby_record_euler_at_present(struct CPUState* cpu, uint32_t moby,
                                 ngte_euler_sc* e);

/** @brief The euler angles a record displays at this present, as fractional byte angles {Z, X, Y}.
 *
 * The angles moby_record_euler_at_present samples the table at, before its
 * rounding to integer sine and cosine: each axis the shortest arc between the
 * pair's previous and current byte at the pair's phase, under the same
 * conditions.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param ang Receives {Z, X, Y}; left untouched when 0 is returned.
 * @return 1 when the pair supplies the angles, else 0 (the caller takes the m_Rotation word). */
int moby_record_angles_at_present(struct CPUState* cpu, uint32_t moby, double ang[3]);

/** @brief A Moby's own rotation (the five words at game_layout.moby.rot_off) at this present, at unit scale.
 *
 * Under the conditions moby_record_euler_at_present samples its angles, the
 * rotation blends per element between the object's previous and current
 * snapshot at the same phase, so it turns with the record's mesh and holds
 * where the mesh holds (a jump hold pins the phase at 0, a facing reversal
 * steps). Otherwise, on every canonical present among them, it is the live
 * matrix the tick left. At phase 1 the blend is the current snapshot itself.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param R Receives the rotation, 1.0 = 1.
 * @return 1 when the snapshot pair supplied it, 0 for the live matrix. */
int moby_env_matrix_at_present(struct CPUState* cpu, uint32_t moby, float R[3][3]);

/** @brief Set the occlusion cull's pan allowance: the camera's rotation this tick as screen-pixel motion.
 *
 * The value is rad/tick times an overestimated pixels-per-radian. The feeder
 * inflates a cached screen rect by it, scaled by the view's H scale, per tick
 * of cache age, so a pan cannot cull a moby whose stale rect has swept toward
 * a wall edge.
 * @param px_per_tick The allowance in pixels per tick at the canonical H. */
void moby_native_set_occl_pan(float px_per_tick);

/** @brief The mobys the canonical render passes drew this tick, taken from their cull records and deduplicated across the material passes.
 *
 * Most are also reachable by walking the moby array, but not all: a game may
 * relocate a moby into render scratch and draw it from there, and a moby
 * drawn from outside the array has no prev/curr snapshot and no fractional
 * position, so it would step at the tick rate while everything around it
 * interpolates. The interp capture and the sub-tick lerp walk consult this
 * list so that class is covered generally. A set older than two VBLs dates
 * from before a stall (a CD load or level swap) and reports no entries, since
 * its addresses may name other objects.
 * @param out Receives the borrowed array.
 * @return The entry count. */
int moby_native_canon_drawn_mobys(const uint32_t** out);

/** @brief The mobys the last host reveal walk admitted (moby_game.host_reveal), the ones no cull record holds.
 *
 * The set is the walk's published hysteresis set: the admitted mobys that
 * drew plus the admitted ones the occlusion cull dropped, without a moby
 * admitted only because its extent sphere straddles the camera plane. A walk
 * that returns before admitting (a canonical view without --no-lod, or a
 * cutscene that owns the draw) leaves the previous walk's set in place.
 * Reports no entries when the regular pass last ran more than two VBLs ago,
 * the bound moby_native_canon_drawn_mobys keeps. Empty on a game without
 * host_reveal.
 * @param out Receives the borrowed array.
 * @return The entry count. */
int moby_native_revealed_mobys(const uint32_t** out);

/** @brief The mobys the reveal walk of the last canonical regular pass admitted, under the tick's own camera.
 *
 * The moby_native_revealed_mobys set as the canonical pass published it,
 * latched before any sub-tick present republishes it; empty when that pass's
 * walk admitted nothing or returned early (a canonical view without
 * --no-lod, a cutscene that owns the draw, no native scene feed). Together
 * with moby_native_canon_drawn_mobys it is the set of mobys the native view
 * drew on the tick. Reports no entries when the pass ran more than two VBLs
 * ago.
 * @param out Receives the borrowed array.
 * @return The entry count. */
int moby_native_view_revealed_mobys(const uint32_t** out);

/** @brief The mobys the simple models' queue of the current canonical epoch holds, reveal records included.
 *
 * Empty on a game without a simple pass, and before this epoch's queue is
 * captured.
 * @param out Receives up to @p max moby addresses.
 * @param max Capacity of @p out.
 * @return The count written. */
int moby_native_simple_mobys(uint32_t* out, int max) OP_REQUIRES(sim);

/** @brief Whether the game's cutscene renderer owns the draw, so it draws its own actor set and no other moby.
 *
 * The game's rule (moby_game.cutscene_owns_draw; Spyro 1: g_GameState 8 with
 * g_DragonCutscene.m_State 1..6). A game without one answers 0.
 * @param cpu The guest CPU state, read only.
 * @return Nonzero while the cutscene renderer owns the draw. */
int moby_native_cutscene_owns_draw(CPUState* cpu);

/** @brief Sub-tick re-extract entry: rebuild this present's moby contribution from the captured cull records and the interpolated camera.
 *
 * Inert while the native scene is off or the canonical present did not draw
 * the moby render path. Each material pass (regular, specular, simple,
 * shaded) replays only when its own producer ran in the committed
 * canonical epoch, so a state that ran a subset of the draws replays that
 * subset. The paint key is not computed while the rank carry applies the
 * tick's verdict.
 * @param cpu The guest CPU state. */
void moby_reextract(struct CPUState* cpu) OP_REQUIRES(sim);
/** @brief Feed the overlay pass held for this canonical present, rebuilt from the overlay moby's current state.
 *
 * The menu screens' one-moby overlay pass (game_layout.moby.overlay_moby) runs
 * before the menu update moves that moby, so the pass opens its scene and
 * holds its feed. Called once per canonical present, before the scene is
 * composed: the overlay moby's record is rebuilt through the cull's rule
 * (moby_game.reveal_record) in place of the pass's record in the host record
 * copy, which the sub-tick re-extract replays, and walked into the scene the
 * pass opened. When a later cull of the same Draw replaced the copy, the walk
 * runs in a borrowed copy holding the pass's record, the later copy is
 * restored, and the rebuilt record is kept for the re-extract to replay as
 * the overlay pass. The pass's own record is fed instead when the overlay word names
 * another moby or the builder declines; nothing is fed when the scene closed
 * first. Reads guest memory only. No effect on a game that names no overlay
 * word. */
void moby_native_overlay_commit(void) OP_REQUIRES(sim);
/** @brief Whether the simple models have a queue this present may replay.
 *
 * True when the game provides the kind's replay (moby_game.simple_replay),
 * the canonical present ran its draw, and that draw's epoch is the committed
 * one. The queue is a host copy rather than a staged view of
 * the tick's draw scratch, so it replays independently of the animated queue's
 * staging: a state whose tick ran one of the two draws replays that one, and a
 * state that ran neither still replays nothing. While a held cut's
 * previous-queue replay is in progress the kind replays whether or not tick N
 * ran its draw, since the replay shows tick N-1's copy (cut_held_set_pick).
 * @return 1 when the queue may replay, else 0. */
int moby_native_simple_fresh(void);

/* ---- simple models ----------------------------------------------------
 * A game whose moby set includes a model kind its own draw pass owns
 * (game_layout.moby.hook_draw_simple) decodes that kind inside
 * moby_game.simple_pass and hands the result back through these calls: one
 * per record to name the moby the following geometry belongs to, one per
 * record to project its model-space vertices, then one per triangle naming
 * three of those vertices. The projection (moby_project_vertex, the routine
 * every moby pass shares), the GTE near-plane mirror, the per-face identity
 * tagging and the scene push stay with the shared walk, so the simple mobys
 * land in the same scene category, the same depth space and the same
 * projection store as the animated one. Valid only from inside simple_pass
 * or simple_replay.
 *
 * The naming and projection calls run on the sim thread, record by record.
 * After each record's projection the provider takes its scope
 * (moby_simple_scope_take); once every record is projected it hands the
 * scopes to moby_simple_fanout, which runs the provider's per-record job,
 * possibly on the walk workers, with an emit context bound to that record.
 * The triangle, facing and world-bucket calls take that context and read no
 * guest memory. */
/** @brief Name the moby the following simple-model triangles belong to.
 *
 * Sets the current record's moby, class, HUD tag (static or UI address
 * range) and cutscene-actor front flag, and clears the screen-space origin
 * and the per-face emit keys and world bucket.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address, or 0. */
void moby_simple_record(struct CPUState* cpu, uint32_t moby);
/** @brief Set the projection origin of a record whose model the game places in screen space rather than in the world.
 *
 * The following triangles of this record project through this origin, in PSX
 * pixels, under the canonical view, which keeps the game's projection
 * distance, in place of the view's own origin. The record is tagged HUD3D by
 * the call. Called after moby_simple_record, which clears it, so a
 * world-placed record projects through its view's origin.
 * @param ofx The origin X, PSX pixels.
 * @param ofy The origin Y, PSX pixels. */
void moby_simple_screen_origin(double ofx, double ofy);
/* One simple-moby record's transform as the draw loads it: the rotation in
 * Q12 for the paint key's integer RTPS, the same rotation at unit scale in
 * float and the translation at full precision for the projection, and the
 * integer translation the RTPS loads. TR and itr are in the draw's GTE
 * units, half a world unit. */
typedef struct {
    const int (*R)[3];          /* the composed rotation, 4096 = 1.0 */
    const float (*Rf)[3];       /* the same rotation at unit scale */
    const double* TR;           /* the translation, world / 2 */
    const int* itr;             /* the integer translation the draw's RTPS loads */
    const moby_otk_rec_t* otk;  /* the record's paint key (moby_simple_otk_record) */
} moby_simple_xform_t;
/** @brief Project the current simple-moby record's model-space vertices into the shared projection store.
 *
 * Each vertex runs moby_project_vertex under the record's view, with the
 * record's rotation and translation in the draw's GTE units and W factor 2,
 * so the GTE divide-overflow test reads the GTE's own depth and the written
 * depth and perspective W are full world.
 * The vertices land in the store's simple-model region, where the following
 * moby_simple_tri calls name them and the stage readback captures them, or
 * in a scratch array, uncaptured, once the region is full. Runs the
 * occlusion audit of the record (OPENPETE_MOBY_OCCL_VERIFY).
 * @param x The record's transform.
 * @param vm The model-space vertices.
 * @param n Their count, at most MOBY_SIMPLE_MAX_VERTS.
 * @param scr Receives each vertex's screen position after the GTE mirror.
 * @param iv Receives each vertex's integer RTPS for the paint key; written
 *        only when the paint key carries a row. */
void moby_simple_project_record(const moby_simple_xform_t* x, const double (*vm)[3], int n,
                                float (*scr)[2], moby_otk_vert_t* iv);
/* The vertex count moby_simple_project_record takes per record. */
#define MOBY_SIMPLE_MAX_VERTS 256
/* One simple-model record's projection, its slots and terms taken on the sim
 * thread (moby_simple_project_defer) and its vertex loop run later by the
 * record's job (moby_simple_project_run). The transform's pointers name the
 * provider's per-record storage, which must outlive the jobs. */
typedef struct {
    moby_simple_xform_t x;
    const ngte_view* view;
    double ofx, ofy;
    uint32_t gsab_flags;
    void* out;          /* the record's slots in the projection store */
    int n;
    int check;          /* the compare mode: the prefix projected already, the job compares */
} moby_simple_proj_t;
/** @brief Take the current simple-model record's projection slots and terms, leaving the vertex loop to the record's job.
 *
 * The serial half of moby_simple_project_record: the slots, the record's
 * class bits and the scope's vertex pointer are as that call leaves them, so
 * moby_simple_scope_take follows either. Refuses (returns 0, nothing taken)
 * under OPENPETE_MOBY_SIM_PROJ_DEFER=0, while the stage readback, the
 * occlusion audit or the late trace is on, and when the record would land in
 * the scratch array (the store's region full); the caller then projects with
 * moby_simple_project_record. Under OPENPETE_MOBY_SIM_PROJ_DEFER_CHECK the
 * projection also runs here and moby_simple_project_run compares its own
 * against it, counting records, vertices and mismatches into the exit line
 * `[sim-proj-defer-check final]`.
 * @param x The record's transform; its pointers must stay valid until the job ran.
 * @param n The vertex count, at most MOBY_SIMPLE_MAX_VERTS.
 * @param vm The model-space vertices, read only in the compare mode.
 * @param iv Receives the integer RTPS in the compare mode, as moby_simple_project_record writes it.
 * @param pj Receives the deferred projection.
 * @return 1 when deferred, 0 when the caller must project now. */
int moby_simple_project_defer(const moby_simple_xform_t* x, int n, const double (*vm)[3],
                              moby_otk_vert_t* iv, moby_simple_proj_t* pj);
/** @brief Run a deferred simple-model projection into its slots, on any thread.
 *
 * Writes the record's slots and iv only; the result is what
 * moby_simple_project_record writes for the same record. In the compare
 * mode it projects into the worker's scratch and compares with the slots.
 * @param pj The deferred projection.
 * @param vm The model-space vertices.
 * @param iv Receives each vertex's integer RTPS when the paint key carries a row.
 * @param worker The job's scratch index, 0 to MOBY_SIMPLE_CHECK_WORKER. */
void moby_simple_project_run(const moby_simple_proj_t* pj, const double (*vm)[3], moby_otk_vert_t* iv,
                             int worker);
/** @brief The occlusion verdict for one simple-moby record from its view-space origin and bounding radius.
 *
 * The record's bounds are the screen rect of the cube around the sphere of
 * that radius and the sphere's nearest view-Z, under the pass view at this
 * present's camera; the verdict is the grid query the animated models' cached
 * bounds take, decided by the canonical scene and held per epoch for every
 * replay of it in tables of this kind's own. A HUD or UI moby, a sphere
 * reaching within 64 view-Z or half the canonical H of the camera, and
 * OPENPETE_MOBY_SIMPLE_OCCL=0 are never occluded. Under
 * OPENPETE_MOBY_OCCL_VERIFY a culled record is reported kept, so the decode
 * runs and the record's projection audits it.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param org The record's origin, view space, full world units.
 * @param radius A bound on the distance of every vertex of the record from
 *        the origin, full world units.
 * @return 1 when the record is hidden behind nearer world depth, else 0. */
int moby_simple_occluded(struct CPUState* cpu, uint32_t moby, const double org[3], double radius) OP_REQUIRES(sim);
/* One simple-model record's emit scope, taken after its projection: where its
 * projected vertices are and the record tags every triangle carries. Owned by
 * moby.c; the provider only stores it. */
typedef struct {
    void*    proj;     /* the record's vertices in the projection store, or NULL */
    int      ovf;      /* their run in the overflow copy when the store was full, else -1 */
    uint32_t moby;
    uint16_t cls;
    int8_t   dofs;
    uint8_t  ui, front, single, cb_rec;
} moby_simple_scope_t;
/* An emit context bound to one simple-model record (moby_simple_fanout). */
typedef struct moby_simple_emit moby_simple_emit;
/* The walk workers a simple-model job may run on: scratch indices 0 to
 * MOBY_SIMPLE_WORKERS - 1, plus MOBY_SIMPLE_CHECK_WORKER, the serial run of
 * the identity check (OPENPETE_MOBY_SIMPLE_FANOUT_CHECK), whose counters the
 * provider does not sum. */
#define MOBY_SIMPLE_WORKERS 8
#define MOBY_SIMPLE_CHECK_WORKER MOBY_SIMPLE_WORKERS
/* The provider's per-record job: the face walk and emit of record `rec`
 * through `se`, with scratch index `worker`; it reads no guest memory and
 * writes only that scratch and what `se` emits. */
typedef void (*moby_simple_job_fn)(moby_simple_emit* se, int rec, int worker, void* arg);
/** @brief Take the current simple-model record's emit scope, after its moby_simple_project_record.
 *
 * Records the moby, class, OT bias, HUD tag, front flag, single-bucket flag
 * and class bits the naming and projection set, and where the record's
 * projected vertices are; a record projected into the scratch array (the
 * store's region full) is copied out so each keeps its own until its emit.
 * @param n The record's vertex count, as passed to the projection.
 * @param sc Receives the scope. */
void moby_simple_scope_take(int n, moby_simple_scope_t* sc);
/** @brief Run the provider's job for every record, appending the records' triangles to the walk output in record order.
 *
 * Runs serially on the sim thread's emit context for the first decode, under
 * OPENPETE_MOBY_WALK_SERIAL, below its own fan-out face bound
 * (OPENPETE_MOBY_SIMPLE_FANOUT_MIN), with one worker or fewer than two records, and
 * while a walk instrument is on; otherwise one record per work-pool item, each
 * worker emitting into its slab, merged in record order as the animated walk
 * merges, its walk diagnostics summed into the serial slot. Either way the
 * stream is the one the serial run emits. Under
 * OPENPETE_MOBY_SIMPLE_FANOUT_CHECK a fanned-out decode also runs serially and
 * compares the two streams' digests, counting the decodes and records
 * compared and the mismatches per leg (canonical, sub-tick).
 * @param sc The records' scopes, one per record.
 * @param nrec The record count.
 * @param faces The faces the jobs will walk, for the fan-out bound.
 * @param fn The job.
 * @param arg Passed to every job. */
void moby_simple_fanout(const moby_simple_scope_t* sc, int nrec, long faces,
                        moby_simple_job_fn fn, void* arg);
/* The simple-model stages a provider times into the moby stage totals
 * (mcan.simple.* / mrex.simple.*). */
enum { MOBY_NATIVE_STAGE_SIMPLE_PREFIX = 0, MOBY_NATIVE_STAGE_SIMPLE_FANOUT = 1 };
/** @brief The moby fine-stage clock: a monotonic nanosecond stamp while a trace is open and OPENPETE_MOBY_SUBSTAGE is on, else 0.
 * @return The stamp, or 0. */
uint64_t moby_native_stage_now(void);
/** @brief Add the nanoseconds since t0 to a simple-model stage of the current leg (canonical or sub-tick).
 * @param which MOBY_NATIVE_STAGE_SIMPLE_PREFIX or MOBY_NATIVE_STAGE_SIMPLE_FANOUT.
 * @param t0 The moby_native_stage_now value at the stage's start; 0 adds nothing. */
void moby_native_stage_add(int which, uint64_t t0);
/** @brief Emit one simple-model triangle over three vertices of the bound record's projection.
 *
 * The vertices are moby_simple_project_record's; the triangle goes through
 * the shared emit with the animated walk's tagging.
 * @param se The emit context bound to the record.
 * @param vi The three vertex indices into the record.
 * @param rgb The three vertex colours.
 * @param u The three texel U coordinates.
 * @param v The three texel V coordinates.
 * @param textured 1 when the triangle is textured.
 * @param tpage_x The texture page X, VRAM pixels.
 * @param tpage_y The texture page Y, VRAM pixels.
 * @param clut_x The CLUT X, VRAM pixels.
 * @param clut_y The CLUT Y, VRAM pixels.
 * @param bpp The texture depth.
 * @param blend 0 = opaque, else the blend id moby_abr_blend maps.
 * @param fkey The face identity key.
 * @param otk The triangle's bucket word in the record's paint order
 *        (moby_rank.h: the face's moby_otk_face_bucket, MOBY_OTK_Q2 set on the
 *        second triangle of one prim), -1 for none. */
void moby_simple_tri(moby_simple_emit* se, const int vi[3], const uint32_t rgb[3],
                     const int u[3], const int v[3],
                     int textured, int tpage_x, int tpage_y,
                     int clut_x, int clut_y, int bpp, int blend,
                     uint32_t fkey, int32_t otk);
/** @brief A simple-model face's two facing values over the bound record's projection, by the animated walk's rule.
 *
 * NCLIP1 is (v0, v1, v2) and NCLIP2 is (v3, v1, v2), on the float screen
 * positions; a face with a vertex in the GTE divide-overflow regime takes
 * both from the homogeneous positions instead, the side of the face's plane
 * the eye is on, which is the geometry its near-clipped draw shows. The
 * vertices are moby_simple_project_record's.
 * @param se The emit context bound to the record.
 * @param idx The face's four vertex indices into the record.
 * @param tri 1 for a triangle, which has no NCLIP2.
 * @param ncl1 Receives NCLIP1.
 * @param ncl2 Receives NCLIP2, -1 on a triangle. */
void moby_simple_face_nclips(const moby_simple_emit* se, const int idx[4], int tri,
                             double* ncl1, double* ncl2);
/** @brief The paint key of the current record under a row, with the TRZ the draw loaded.
 *
 * The row becomes MOBY_OTK_NONE while the rank is off or the present carries
 * the tick's verdict.
 * @param row The record's row.
 * @param trz The GTE TRZ the draw loaded.
 * @param out Receives the paint key. */
void moby_simple_otk_record(moby_otk_row_t row, int trz, moby_otk_rec_t* out);
/** @brief The world ordering-table bucket the following triangles of the bound simple-moby record are linked into.
 *
 * Each following triangle of the record carries it as its contest key's
 * bucket (moby_record_world_buckets for an animated model); binding the
 * context clears it.
 * @param se The emit context bound to the record.
 * @param w The bucket, or -1 for none. */
void moby_simple_world_bucket(moby_simple_emit* se, int w);

/** @brief Offer the mobys the canonical cull rejected to moby_game.reveal_record, which appends records in the game's own encoding.
 *
 * Walks the moby array, skipping the sentinel, dead states, run-once
 * instances and a moby whose key sector the --sector-visibility predicate
 * rejects, and applies three clauses. The widened-view clause (animated
 * models, moby_game.cull_view_admits supplied, view widening above 1) offers
 * a moby the cull left undrawn that the cull's admission admits with the
 * lateral slope scaled to the view. The extended-LOD clause (--no-lod)
 * offers a moby outside the cull's distance box (moby_game.cull_box_threshold)
 * that stands in front of the camera plane. The sector-byte clause (every
 * --lod, moby_game.cull_view_admits supplied) offers a moby inside the box
 * that the cull dropped on its sector byte alone. Every clause skips a moby
 * the occlusion test culls. Any other moby the canonical box admits is left
 * to the cull, which either queued it or rejected it for a reason the box
 * does not express.
 *
 * Reads gamestate only. Call from inside the pass whose queue the game
 * appends to, once per model kind, after that pass has captured the cull's
 * own records and before it decodes them. Inert unless the game supplies
 * cull_box_threshold and reveal_record and one of the clauses applies.
 * @param cpu The guest CPU state, read only.
 * @param kind The model kind whose queue the records join.
 * @return The number of records appended. */
int moby_reveal_candidates(struct CPUState* cpu, moby_reveal_kind_t kind) OP_REQUIRES(sim);

/* ---- decode pieces shared with the player feeder -----------------------
 * The avatar draw and the moby draw are different routines, but they encode a
 * face the same way and run the same emit tree, so these four live here
 * once. */
/** @brief The signed doubled area of a screen-space triangle, the GTE NCLIP analog on float coordinates rather than the GTE's floored int16.
 *
 * (b - a) x (c - a), the sign convention NCLIP uses; the caller reads the
 * sign as the facing.
 * @param ax Vertex a's screen X.
 * @param ay Vertex a's screen Y.
 * @param bx Vertex b's screen X.
 * @param by Vertex b's screen Y.
 * @param cx Vertex c's screen X.
 * @param cy Vertex c's screen Y.
 * @return The signed doubled area. */
double moby_tri_nclip(float ax, float ay, float bx, float by, float cx, float cy);
/** @brief Map a texture page's semi-transparency mode to the renderer's blend id.
 *
 * ABR 0 is average (id 1), 1 additive (2), 2 subtractive (3) and 3
 * quarter-additive (4), the hardware's own table.
 * @param abr The mode; only its low two bits count.
 * @return The blend id, 1..4. Opaque (0) is the caller's, from its own
 *         semi-transparency bit. */
int    moby_abr_blend(int abr);
/** @brief Whether the moby walk subtracts the sub-slot tie pull from each vertex's stored view-Z.
 *
 * On unless OPENPETE_MOBY_OTBIAS or OPENPETE_MOBY_TIE_PULL is set to a value
 * starting with '0'; both are read once and latched.
 * @return 1 when the tie pull applies, else 0. */
int    moby_tie_pull_on(void);
/** @brief Whether opaque actor faces write their ordering-table key (mean view-Z less the authored depth offset) into the actor key image.
 *
 * On unless OPENPETE_MOBY_OTBIAS or OPENPETE_MOBY_FACE_KEY is set to a value
 * starting with '0'; both are read once and latched.
 * @return 1 when the face key channel applies, else 0. */
int    moby_face_key_on(void);
/** @brief Whether the player walk applies its sortDepth pulls: the front-sentinel whole-model near pull, the tie pull, and the mirrored copy's negative bias.
 *
 * On unless OPENPETE_MOBY_OTBIAS or OPENPETE_PLAYER_SORT_PULL is set to a
 * value starting with '0'; both are read once and latched.
 * @return 1 when the sortDepth pulls apply, else 0. */
int    moby_player_sort_pull_on(void);
/** @brief Fill one face record's texture fields from its three texture words.
 *
 * The words are {u0|v0<<8|clut<<16}, {u1|v1<<8|tpage<<16} and
 * {u2|v2<<8|u3<<16|v3<<24}. A tpage colour-depth field of 3 is reserved on the
 * hardware and marks a replaced texture instead: the record takes depth 32 and
 * carries the texture id, the CLUT half read as a signed 16-bit value, where
 * the page x would go.
 * @param td0 The first texture word.
 * @param td1 The second.
 * @param td2 The third.
 * @param F Receives the u/v pairs, blend mode, page, depth and CLUT fields. */
void   moby_tiledef_decode(uint32_t td0, uint32_t td1, uint32_t td2, moby_wface_t* F);
/** @brief Decide from two NCLIP areas which triangles of a face draw, and in which vertex order.
 *
 * A fresh (quad-capable) face tests NCLIP1 on (v0,v1,v2) and NCLIP2 on
 * (v3,v1,v2) and emits, by the sign pair, the triangle (v3,v1,v2), nothing,
 * the quad (v0,v1,v2)+(v1,v3,v2), or the triangle (v0,v1,v2). Without
 * have_v3 the quad and the (v3,v1,v2) triangle degrade to nothing and the
 * triangle (v0,v1,v2). A strip face is one triangle (v0,v1,v2) kept on
 * NCLIP1's sign.
 *
 * The face's own bit0 changes both rules, and the two draws differ in how.
 * With bit0_unconditional the bit skips the test outright: a pre-marked quad
 * is emitted whole (a triangle without have_v3) and a strip triangle is kept
 * of either winding. Without it the bit replaces NCLIP1 by -|NCLIP1| and NCLIP2
 * by |NCLIP2| before the same tests, which differs from the first only on a
 * zero-area face; a strip triangle is then kept unless NCLIP1 is 0.
 * @param fresh Nonzero for a quad-capable face, 0 for a strip face.
 * @param bit0 The face's bit0.
 * @param bit0_unconditional Nonzero when bit0 skips the test.
 * @param ncl1 NCLIP of (v0,v1,v2).
 * @param ncl2 NCLIP of (v3,v1,v2); unused for a strip face.
 * @param have_v3 Nonzero when the face has a fourth vertex; unused for a strip
 *        face.
 * @param order Receives the three face-vertex slots (0..3) of triangle e in
 *        order[e].
 * @return The triangle count, 0..2. */
int    moby_face_emit(int fresh, int bit0, int bit0_unconditional,
                      double ncl1, double ncl2, int have_v3, int order[2][3]);

/** @brief The guest base of the cull-record queue the last canonical moby pass read, 0 before any pass.
 *
 * A game whose queue lives in memory the rest of the tick reuses stages the
 * queue's bytes for each sub-tick present, so the replay reads the queue the
 * tick drew rather than whatever overwrote it. The base is captured at the
 * pass rather than recomputed, since a queue carved out of the polybuf rides
 * the draw buffer that tick allocated.
 * @return The base, or 0. */
uint32_t moby_canon_rec_base(void);
/** @brief The length in bytes of the queue moby_canon_rec_base names, game_layout.moby.rec_base_polybuf_off.
 *
 * A queue carved out of the polybuf runs from its base to the polybuf end.
 * 0 for a game that keeps its queue at a fixed address nothing else touches.
 * @return The length, or 0. */
uint32_t moby_canon_rec_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_MOBY_H */
