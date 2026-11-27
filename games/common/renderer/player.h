/* renderer/player.h - the shared player-avatar feeder's entries.
 *
 * The walk lives in games/common/renderer/player.c and reads every engine
 * address from game_layout.player; the per-game stream formats, the skeleton
 * transform and the game's own draw come through player_game
 * (renderer/player_game.h). A game reaches the feeder by listing player_module
 * in its feature list; a game whose layout names no draw hook registers
 * nothing and the walk never runs.
 *
 * The record store below holds one record per draw-hook call. Every canonical
 * draw-hook call appends a record (the state bytes the extractor reads, the
 * part-transform block, the projected-SXY block
 * and the polybuf range) to the epoch's list. The
 * canonical-epoch commit closes the list into a three-deep store (curr, prev,
 * prev2) with one clock per store, pairs each record with a record of the
 * previous list (index first, position-verified within the layout's pair
 * bound; a second pass by nearest position), and appends a pairing-only record
 * when no hook observed the ring's tick-end avatar. The sub-tick re-extract
 * runs the feeder once per record, staging that record's bytes and brackets.
 * With one call per tick only record k=0 exists.
 *
 * The auxiliary feeders (drop shadow, flame) append their own records bound to
 * the model record of the same epoch whose position equals the live one at the
 * hook, so a game that draws several avatars in one tick keeps each avatar's
 * shadow and flame with its own model record. */
#ifndef OPENPETE_COMMON_RENDERER_PLAYER_H
#define OPENPETE_COMMON_RENDERER_PLAYER_H

#include <stdint.h>
#include "openpete_time.h"   /* op_time_t */
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

enum { PLAYER_REC_MODEL = 0, PLAYER_REC_SHADOW = 1, PLAYER_REC_FLAME = 2,
       PLAYER_REC_FEEDERS = 3 };

#define PLAYER_REC_MAX        16      /* per feeder per epoch */
#define PLAYER_REC_STATE_MAX  0x2E8u  /* the largest state span a record copies */
#define PLAYER_REC_PART_MAX   0x100u  /* the part-transform block */
#define PLAYER_REC_SXY_MAX    0x400u  /* the projected-SXY block */
#define PLAYER_REC_AUX_MAX    0xD0u   /* the largest auxiliary feeder's state */

typedef struct {
    uint8_t  spyro[PLAYER_REC_STATE_MAX];  /* the hook's state block: the pairing key across lists */
    uint8_t  src[PLAYER_REC_STATE_MAX];    /* the shown state, staged and lerped by the re-extract:
                                            * the same bytes, except under --runahead where the led
                                            * list's record (the tick the canonical present shows)
                                            * goes here; seg and sxy follow it */
    uint8_t  seg[PLAYER_REC_PART_MAX];
    uint8_t  sxy[PLAYER_REC_SXY_MAX];
    uint32_t rp_start, rp_end;   /* polybuf range of the call (0,0 = pairing-only) */
    uint8_t  drawn;              /* 1 = a hook call; 0 = pairing-only (never rendered) */
    uint8_t  is_player;          /* 1 = the call the gameplay drives; 0 = a duplicate copy */
    int8_t   pair;               /* index in the previous list, -1 unpaired */
    int8_t   pair2;              /* index in the list before that (via pair), -1 */
} player_model_rec_t;

/* A record of an auxiliary feeder (shadow, flame): its state block bound to
 * the model record it renders under. */
typedef struct {
    uint8_t  st[PLAYER_REC_AUX_MAX];   /* the feeder's state at the hook: the list comparison key */
    uint8_t  src[PLAYER_REC_AUX_MAX];  /* the shown state, staged and lerped (led under --runahead) */
    int8_t   model;                    /* index of the model record it renders under */
    uint8_t  drawn;                    /* 1 = a hook call; 0 = pairing-only */
} player_aux_rec_t;

typedef player_aux_rec_t player_shadow_rec_t;
typedef player_aux_rec_t player_flame_rec_t;

/* ---- orchestrator inputs (set per record, read by the re-extract) ------ */

/** @brief Sets the sub-tick alpha and whether keyframe-blend interpolation is active.
 *
 * The re-extract runs the two-pass shape lerp only while `on` is set and a
 * prev/curr part-block pair is held (player_native_set_anim_pair).
 * @param alpha The sub-tick phase the projected vertices lerp at.
 * @param on Nonzero when the camera is bracketed and the record paired. */
void player_native_set_anim_interp(double alpha, int on);
/** @brief Sets the part-block pair the two-pass shape lerp blends.
 *
 * The pointers are held, not copied, and must stay valid through the
 * re-extract. With prev NULL the record renders from curr alone; with both
 * NULL no pair is held.
 * @param prev_seg The paired previous record's block, or NULL when unpaired.
 * @param curr_seg This record's block, or NULL. */
void player_native_set_anim_pair(const uint8_t* prev_seg, const uint8_t* curr_seg);
/** @brief Sets the record index, which is the PLAYER tag slot of the triangles the walk emits.
 *
 * @param k The record index; a negative value (a refused append) is stored as 0. */
void player_native_set_player_slot(int k);
/** @brief Rotates the avatar record's camera-relative position into view space as the avatar draws' ordering MVMVA does.
 *
 * The deltas are (x - cam.x, cam.y - y, cam.z - z) from the record at
 * game_layout.player.base and the camera position at cam.base+0x28; the
 * draw loads them into IR3, IR1 and IR2 (16 bits each) and runs an sf = 1
 * MVMVA by the camera matrix at cam.base+0x00 with no translation, so
 * mac = (R . IR) >> 12 with IR = (d[1], d[2], d[0]) truncated to int16.
 * @param cpu Reads the record and the camera.
 * @param d Receives the three untruncated deltas in that order.
 * @param mac Receives MAC1, MAC2 and MAC3. */
void player_view_mac(struct CPUState* cpu, int32_t d[3], int32_t mac[3]);
/** @brief Keys the group of calls that share the front-layer flatten cap.
 *
 * The cap is shared by the calls of one tick or present. A key different from
 * the current one moves the current group's call count and minimum to the
 * previous group and opens an empty group; the same key is a no-op. The draw
 * hook keys by VBL count (even keys); a replay that shares the cap across a
 * present keys by present serial (odd keys).
 * @param key The group key. */
void player_native_set_flat_key(uint32_t key);
/** @brief Sets the per-part float byte-angles the orchestrator interpolated from the prev/curr euler snapshots.
 *
 * The composer then renders rotation below the 8-bit euler grid
 * (player_rot_interp). Both arrays are copied for PLAYER_PARTS_MAX parts.
 * @param active Per-part gates, or NULL to turn every part off so the
 *        canonical inline render stays byte-exact.
 * @param ang Row-major PLAYER_PARTS_MAX x 3 byte-angles [part][Z, X, Y]; not
 *        read when `active` is NULL. */
void player_native_set_rot_interp(const int* active, const double* ang);

/** @brief Sets the mirrored-copy latch that player_mirror_pass returns.
 *
 * The draw hook sets it from player_game.call_is_mirror at each call; the
 * sub-tick replay sets it from the replayed record's own answer, which the
 * caller read off the staged bytes. A provider's transform reads it to apply
 * the draw's own reflection, and the walk negates both NCLIP results under it.
 * @param on Nonzero for the mirrored copy. */
void player_native_set_mirror(int on);
/** @brief Whether the current call or replayed record is the mirrored copy.
 *
 * Returns the latch player_native_set_mirror last set.
 * @return 1 for the mirrored copy, else 0. */
int  player_mirror_pass(void);

/** @brief The interpolated byte-angles for one part, read by a provider's euler composer.
 *
 * @param part The part index.
 * @param out Receives [Z, X, Y] byte-angles when the part is gated on by
 *        player_native_set_rot_interp.
 * @return 1 when `out` was written; 0 when the part is out of range or gated
 *         off, and the composer uses the canonical byte-exact table lookup. */
int player_rot_interp(int part, double out[3]);

/** @brief One seam word of the part block the walk is reading, for a provider whose part_record fills player_record_t.seam.
 *
 * The two-pass shape lerp swaps the whole block per pass, so each pass places
 * the child part from the keyframe pair of its own pose.
 * @param k 0 for keyframe A, 1 for keyframe B.
 * @return The word; 0 when the game composes no block, declares no
 *         part_seam_off, the word lies past part_block_len, or `k` is not 0 or 1. */
uint32_t player_rec_seam_word(int k);

/** @brief Sub-tick re-extract of the avatar, not mirrored, from the staged gamestate and the interpolated camera.
 *
 * Equals player_reextract_record(cpu, 0): the form an orchestrator with no
 * per-record loop replays. Extract-only.
 * @param cpu Reads the staged gamestate. */
void player_reextract(struct CPUState* cpu) OP_REQUIRES(sim);
/** @brief Sub-tick re-extract of one record of the store, feeding the scene.
 *
 * Returns without feeding when the native scene is off or the canonical tick
 * did not draw the avatar (render_path_canon_drawn). With keyframe-blend
 * interpolation on and a prev/curr part-block pair held, projects the previous
 * and the current pose and lerps the screen positions and view-Z by the set
 * alpha when both passes project the same vertex count; a derived block is
 * swapped through the walk's host copy, a guest block through the sub-tick
 * view and then restored. Otherwise the walk runs once on the staged state.
 * @param cpu Reads the staged gamestate.
 * @param mirror The record's own answer to player_game.call_is_mirror, read by
 *        the caller from the staged bytes, since the driver has already
 *        restored the live word. */
void player_reextract_record(struct CPUState* cpu, int mirror) OP_REQUIRES(sim);

/* ---- record store, canonical side (appends on the game fiber, commit on the
 * host loop) ------------------------------------------------------- */

/** @brief Appends the draw hook call's model record to the epoch's list.
 *
 * Copies the state block (as both the pairing key and the shown state), the
 * part-transform block (derived from gamestate when the provider answers
 * part_record) and the projected-SXY block, and
 * marks the record drawn with its is_player answer. A speculative fire under
 * runahead appends to the led list instead of the canonical one. Called by the
 * shared hook; an auxiliary feeder calls its own append below.
 * @param cpu Reads the part block base.
 * @param rp_start The call's polybuf start.
 * @param rp_end The call's polybuf end.
 * @return The record's index (the tag slot), or -1 when the list holds
 *         PLAYER_REC_MAX records or the state block or part block has no host
 *         pointer. */
int  player_records_model_append(struct CPUState* cpu, uint32_t rp_start, uint32_t rp_end) OP_REQUIRES(sim);
/** @brief Append the live player shadow record to the epoch's list, bound to its model record.
 *
 * Called from the canonical shadow hook. The shadow binds to the newest model
 * record whose position matches the live player (bind_model), appending a
 * pairing-only model record when none does.
 * @param cpu Unused.
 * @return The bound model record's index, or -1 when the game names no
 *         shadow block, the list is full, or the player or shadow block does
 *         not map for its length. */
int  player_records_shadow_append(struct CPUState* cpu) OP_REQUIRES(sim);
/** @brief Append the live player flame record to the epoch's list, bound to its model record.
 *
 * As player_records_shadow_append, for the flame block.
 * @param cpu Unused.
 * @return The bound model record's index, or -1 when the game names no
 *         flame block, the list is full, or the player or flame block does
 *         not map for its length. */
int  player_records_flame_append(struct CPUState* cpu) OP_REQUIRES(sim);
/** @brief Close the hooks' record epoch, once per canonical present, after the interp ring captured the tick.
 *
 * Takes the epoch's appended list when its stamp is the fresh canonical epoch,
 * else an empty list, and empties the append list. An interp-ring reset
 * (savestate load, rewind, runahead depth switch) invalidates the three-deep
 * store first. When no model record holds the ring's tick-end position, a
 * pairing-only model record is appended from the ring's bytes, and shadow and
 * flame pairing-only records for the ring's avatar when their hooks did not
 * fire; pairing-only records are never rendered. Under --runahead each record's
 * shown state (src, seg, sxy) is taken from the led list when that list
 * has the committed list's shape. The list becomes the store's newest entry and
 * is paired against the previous one only when the ring rolled or the list
 * differs from the newest; otherwise it is dropped as a duplicate. Returns at
 * once when no player layout is registered. */
void player_records_epoch_commit(void) OP_REQUIRES(sim);

/* ---- record store, present side (host loop) --------------------------- */

/** @brief Opens the store for one present and sets the alpha the records lerp at.
 *
 * Nothing is opened when the store's newest entry is invalid or not from the
 * fresh canonical epoch. Records lerp only when the ring is paired, not cut,
 * and the store holds a previous entry. The alpha is the ring's when the
 * store's clock equals the ring's current tick; otherwise it is (present
 * clock - store clock) / (store clock - previous clock), the period capped at
 * one tick and the result clamped to -0.35..1.
 * @param ring_paired Nonzero when the interp ring holds a prev/curr pair.
 * @param ring_cut Nonzero on a camera cut.
 * @param ring_alpha The ring's sub-tick alpha.
 * @param out_alpha Receives the records' alpha, 0 when they do not lerp; may be NULL.
 * @return The model record count this present, 0 when nothing is opened. */
int  player_records_present_begin(int ring_paired, int ring_cut, double ring_alpha,
                                  double* out_alpha);
/** @brief Make the next player_records_present_begin open the previous canonical epoch's records, for a held camera cut.
 *
 * The present then shows that epoch's record set with its own drawn verdicts
 * and states, unlerped: the newest store entry when the last commit did not
 * roll the store, else the entry before it. Set before the replay and cleared
 * after it.
 * @param on Nonzero to open the previous epoch's records. */
void player_records_set_hold(int on);
/** @brief The store slot the open held present shows, as a cut_held_pick_t, or -1 outside a hold.
 *
 * Written by player_records_present_begin under player_records_set_hold and
 * cleared by player_records_set_hold; the argument
 * hook_reextract_ready_held takes for a producer bound to the player records.
 * @return CUT_HELD_EMPTY, CUT_HELD_CURR, CUT_HELD_PREV, or -1. */
int  player_records_held_pick(void);
/** @brief The newest store entry's model record k of the open present.
 *
 * Valid between player_records_present_begin and player_records_present_end.
 * @param k The model record index.
 * @return The record, or NULL when `k` is out of range or no present is
 *         open. */
const player_model_rec_t* player_records_curr(int k);
/** @brief The record of the previous list that model record k pairs with.
 *
 * @param k The model record index.
 * @return The paired record, or NULL when record `k` does not exist, is
 *         unpaired, or the previous entry is invalid. */
const player_model_rec_t* player_records_prev(int k);
/** @brief The record two lists back that model record k pairs with.
 *
 * Reached through the previous record's own pairing.
 * @param k The model record index.
 * @return The record, or NULL when `k` does not exist, has no second pairing,
 *         or the oldest entry is invalid. */
const player_model_rec_t* player_records_prev2(int k);
/** @brief Whether model record k lerps against its paired previous record.
 *
 * @param k The model record index.
 * @return 1 when the record exists, player_records_present_begin enabled
 *         lerping, and the record is paired; else 0. */
int  player_records_lerps(int k);
/** @brief Whether a model record of the list being built was drawn by a hook call, as opposed to created for pairing alone.
 *
 * Reads the list the auxiliary binds go into (the led list on a speculative
 * fire), so an auxiliary feeder hooking later in the same tick takes the
 * avatar's own verdict, the one the replay loop applies when it decides
 * whether to reproduce that record.
 * @param k The model record index.
 * @return 1 when drawn, 0 when pairing-only or out of range. */
int  player_records_model_drawn(int k);
/** @brief The shadow record count of the open present.
 *
 * @return The count, 0 when no present is open. */
int  player_records_shadow_count(void);
/** @brief The newest store entry's shadow record j of the open present.
 *
 * @param j The shadow record index.
 * @return The record, or NULL when `j` is out of range or no present is
 *         open. */
const player_shadow_rec_t* player_records_shadow_curr(int j);
/** @brief The previous list's shadow record paired with shadow record j.
 *
 * The one bound to the model record that record j's model pairs with.
 * @param j The shadow record index.
 * @return The record, or NULL when `j` does not exist, its model record is
 *         unpaired, or the previous list has no shadow bound to the pair. */
const player_shadow_rec_t* player_records_shadow_prev(int j);
/** @brief Prints one shadow record's binding and lerp state to stderr.
 *
 * Does nothing unless OPENPETE_PLAYER_RECDBG is set and record `j`
 * exists.
 * @param j The shadow record index.
 * @param staged_anchor The staged shadow anchor's three words, or NULL to print
 *        zeros. */
void player_records_shadow_note(int j, const int32_t* staged_anchor);
/** @brief The flame record count of the open present.
 *
 * @return The count, 0 when no present is open. */
int  player_records_flame_count(void);
/** @brief The newest store entry's flame record j of the open present.
 *
 * @param j The flame record index.
 * @return The record, or NULL when `j` is out of range or no present is
 *         open. */
const player_flame_rec_t* player_records_flame_curr(int j);
/** @brief The previous list's flame record paired with flame record j.
 *
 * The one bound to the model record that record j's model pairs with.
 * @param j The flame record index.
 * @return The record, or NULL when `j` does not exist, its model record is
 *         unpaired, or the previous list has no flame bound to the pair. */
const player_flame_rec_t* player_records_flame_prev(int j);
/** @brief Prints one flame record's binding and lerp state to stderr.
 *
 * Does nothing unless OPENPETE_PLAYER_RECDBG is set and record `j`
 * exists.
 * @param j The flame record index.
 * @param staged_origin The staged flame origin's three words, or NULL to print
 *        zeros. */
void player_records_flame_note(int j, const int32_t* staged_origin);
/** @brief Prints one model record's pairing and lerp state to stderr.
 *
 * Does nothing unless OPENPETE_PLAYER_RECDBG is set and record `k`
 * exists.
 * @param k The model record index.
 * @param staged_pos The staged position's three words, or NULL to print
 *        zeros. */
void player_records_present_note(int k, const int32_t* staged_pos);
/** @brief Closes the store for the present.
 *
 * Clears the open record count and the lerp flag, so the accessors return NULL
 * or 0 until the next player_records_present_begin. */
void player_records_present_end(void);
/** @brief The store's newest clock stamp.
 *
 * @return The stamp, or 0 when the store holds no entry. */
op_time_t player_records_curr_ns(void);
/** @brief Whether OPENPETE_PLAYER_RECDBG is set, latched on the first call.
 *
 * @return 1 when set, else 0. */
int  player_records_dbg(void);

/* ---- introspection, for a game's own numerical harness ----------------- */

/** @brief Decodes and projects every part's vertices from the current pose.
 *
 * Takes the water plane from the provider unless the negative control
 * disables it, derives the part block from gamestate where the provider
 * answers part_record (unless a replay pinned it), derives the part
 * transforms under the current view, and projects each part up to the block's
 * zero flags word, skipping a part with no vertices or no transform.
 * @param cpu Reads the pose and the streams.
 * @return The projected vertex count, which indexes player_proj_vert; 0 when
 *         the provider has no vertex step, the block has no base, or no part
 *         projected. */
int  player_project_all(struct CPUState* cpu);
/** @brief Builds the face-stream walk over the current projection.
 *
 * Decodes each part's face and colour streams once per distinct face stream,
 * runs the dual-NCLIP emit tree on the native float screen positions, applies
 * the water-plane classification and the provider's depth admission, and
 * stores the surviving triangles for player_walk_tri and the scene feed.
 * Returns 0 when nothing is projected or the provider decodes no faces.
 * @param cpu Reads the face and colour streams.
 * @return The number of triangles built. */
int  player_build_walk(struct CPUState* cpu);
/** @brief Reads one vertex of the last projection.
 *
 * An index past the projected count writes nothing; a NULL output is skipped.
 * @param i The vertex index.
 * @param sx Receives the screen X.
 * @param sy Receives the screen Y.
 * @param vz Receives the view-Z.
 * @param sat Receives whether the projection saturated.
 * @param part Receives the part the vertex came from. */
void player_proj_vert(int i, float* sx, float* sy, float* vz, int* sat, int* part);
/** @brief Reads one triangle of the last face-stream walk.
 *
 * An index past the triangle count writes nothing; a NULL output is skipped.
 * @param k The triangle index.
 * @param idx Receives the three stream vertex indices.
 * @param rgb Receives the per-vertex colours.
 * @param u Receives the per-vertex U.
 * @param v Receives the per-vertex V. */
void player_walk_tri(int k, int idx[3], uint32_t rgb[3], int u[3], int v[3]);
/** @brief The transform the provider derived for a part in the last projection.
 *
 * @param p The part index.
 * @return The transform, or NULL when out of range or not derived. */
const struct player_xform* player_part_xform(int p);
/** @brief The part-transform block's guest base this tick.
 *
 * A game with a fixed render scratch names its base (part_base + part_off); a
 * game that carves the scratch out of the polybuf every tick leaves part_base
 * 0 and the block starts at *polybuf.end - part_base_polybuf_off + part_off,
 * recomputed per call.
 * @param cpu Reads the polybuf end pointer.
 * @return The block's guest address, or 0 when the layout names neither form
 *         or the polybuf end is not a RAM address. */
uint32_t player_part_block_base(struct CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_PLAYER_H */
