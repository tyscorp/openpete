/* core/include/openpete_interp.h - render-side state interpolation ring.
 *
 * Captures gameplay-transform state once per VBL the guest advanced so the
 * render path can lerp between the two newest distinct states and present
 * at the host's render rate.
 *
 * Three-stage ring: the last three stored states (prev2, prev, curr).
 * Alpha = (openpete_interp_present_now() - prev.tick_ns) / sim_period_ns,
 * clamped to [0, 1]. Re-emission shows the world at virtual time
 * `prev.tick_ns + alpha * sim_period`, accepting
 * one tick of latency in exchange for never extrapolating. prev2 is kept
 * so the render side can build a cubic (Hermite) interpolant with a
 * centered start-tangent - halving the velocity discontinuity at tick
 * boundaries vs the linear prev->curr chord (see openpete_interp_query3).
 *
 * Scope (this ring): the capture API, the dedupe predicate, and the
 * prev2/prev/curr query (openpete_interp_query3). The lerped re-emission
 * itself - the save-canonical -> write-lerped -> restore bracket around the
 * sub-tick present - is built GAME-SIDE (e.g. reextract.c), NOT
 * here: core stays address-free, and the game owns which regions/feeders it
 * brackets plus its own restore discipline (see the game's bracket-integrity
 * self-check, OPENPETE_BRACKET_CHECK). Capturing here is read-only against
 * PSX RAM and runs only while openpete_interp_set_enabled(1) holds.
 *
 * This DIRECT ring drives the camera/Spyro alpha. Per-OBJECT (indirect pool)
 * state uses a parallel store (openpete_interp_objs.h) with its OWN per-object
 * alpha - intentionally independent of this ring's alpha, so an object that
 * advanced on a tick the camera didn't (idle-homeworld dragon under a static
 * camera, and vice-versa) still interpolates. The two can differ by up to ~1
 * VBL on the same present; that is correct, not a bug.
 *
 * Region table is game-supplied through openpete_interp_register_regions.
 * core/ holds zero game-specific addresses. */

#ifndef OPENPETE_INTERP_H
#define OPENPETE_INTERP_H

#include <stddef.h>
#include <stdint.h>

#include "openpete_time.h"
#include "openpete_thread_roles.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The region-table cap and the maximum total bytes captured per snapshot
 * across all regions. The bytes are set generously to cover Spyro (0x2A4) +
 * Camera (0x110) + later mobies; the three ring slots take ~96 KB in total. */
#define OPENPETE_INTERP_MAX_REGIONS         16
#define OPENPETE_INTERP_MAX_SNAPSHOT_BYTES  (32u * 1024u)

/* Region descriptor. Kept narrower than OpenpeteStateRegion: the interp
 * ring only handles DIRECT regions in v1 - moby pools (indirect arrays)
 * land in task #59 with their own descriptor variant. */
typedef struct {
    const char* name;       /* diagnostic */
    uint32_t    psx_addr;   /* KSEG0 PSX vaddr */
    uint32_t    length;     /* bytes to capture */
    /* Present this region from the COMMITTED timeline under runahead:
     * after the driver rolls guest RAM back from the led speculation head,
     * openpete_interp_recapture_committed() re-reads flagged regions into
     * the ring's newest slot, replacing the led bytes. Use for regions
     * where a misprediction is catastrophic and latency is cheap - the
     * follow-camera is the canonical case: showing it led means every
     * input-edge un-shows N ticks of mispredicted pan (the R2 release
     * snap), while showing it committed merely adds N ticks of follow-lag
     * to an object that trails by design. 0 = led (default). */
    int         ra_commit;
} openpete_interp_region_t;

/** @brief Register the game's direct-region table for capture.
 *
 * Called from the game's startup (games/common/renderer/interp_regions_gen.c)
 * before openpete_interp_init, which sizes the snapshot layout from it.
 * Explicit registration replaces a weak symbol: MinGW's PE/COFF link does not
 * reliably let a strong definition in the executable override a weak one
 * pulled from a static library archive. The last call wins. The pointer and
 * count are stored by value; the caller keeps the table alive.
 * @param regions Region table, or NULL to keep capture disabled.
 * @param count Entries in @p regions; ignored when @p regions is NULL. */
void openpete_interp_register_regions(const openpete_interp_region_t* regions,
                                      size_t count);

/* One ring slot. `data[]` holds the concatenation of each region's bytes
 * in table order; `region_offsets[r]` gives the offset of region r within
 * `data[]`. `valid` flips to 1 the first time this slot is written. */
typedef struct {
    uint32_t  tick;                 /* openpete_frame at capture time */
    op_time_t tick_ns;              /* stamp passed to capture (the VBL clock when deterministic) */
    uint32_t  total_bytes;
    uint8_t   valid;
    uint8_t   pad[3];
    uint32_t  region_offsets[OPENPETE_INTERP_MAX_REGIONS];
    uint8_t   data[OPENPETE_INTERP_MAX_SNAPSHOT_BYTES];
} openpete_interp_snapshot_t;

/** @brief Register the discontinuity handler and lay out the snapshot ring from the region table.
 *
 * Idempotent once it has succeeded. Registers the guest-discontinuity
 * handler that resets this ring and the object table, even with no regions
 * registered (capture then stays a no-op and init succeeds). Computes each
 * region's offset within a snapshot and clears the three slots. A table
 * with more than OPENPETE_INTERP_MAX_REGIONS entries or a byte total over
 * OPENPETE_INTERP_MAX_SNAPSHOT_BYTES logs an error and fails; the caps are
 * raised by editing them, not at run time.
 * @return 0 on success, 1 when the table exceeds a cap. */
int  openpete_interp_init(void);
/** @brief Mark the ring uninitialised and disable capture. */
void openpete_interp_shutdown(void);

/** @brief Invalidate every snapshot slot, keeping the region layout.
 *
 * Also clears the latest-capture tick, restores the distinct-state gap to
 * its floor of 2 and increments the reset generation. Called on a
 * guest-state discontinuity (the handler openpete_interp_init registers:
 * savestate load, rewind restore) and by run.c beside
 * openpete_interp_objs_reset where the timeline jumps otherwise. The stored
 * snapshots describe the pre-jump timeline; dropping them makes the next
 * present snap to the restored state instead of lerping across the jump. */
void openpete_interp_reset(void);
/** @brief Return the count of openpete_interp_reset calls.
 *
 * A game-side store that must reset wherever the ring resets (the player
 * record store in games/common/renderer/player.c) compares this at its own
 * commit instead of registering at each reset site.
 * @return The reset generation, starting at 0. */
uint32_t openpete_interp_reset_generation(void);

/** @brief Enable or disable capture, the queries and the object table.
 *
 * run.c enables it when --render-fps exceeds the sim rate, under offline
 * capture at two or more presents per tick, and for intermediate
 * screenshots, and re-evaluates it when the render rate changes. Capture
 * also needs a successful openpete_interp_init with a non-empty region
 * table; this flag does not check that. Off by default.
 * @param on Nonzero enables. */
void openpete_interp_set_enabled(int on);
/** @brief Return the flag openpete_interp_set_enabled last stored.
 *
 * @return 1 when enabled, else 0. */
int  openpete_interp_enabled(void);

/** @brief Snapshot every registered region into the ring, committing only a new state or a held one.
 *
 * Called from run.c once per VBL the guest advanced (ticked_this_iter),
 * while the game fiber is yielded; reads PSX RAM only. The candidate is
 * built in scratch (an unmapped region reads as zeros) and compared with
 * the ring's head. A differing candidate rolls the ring and, when the head
 * was itself a distinct state that had not held past that gap, updates the
 * measured tick gap between distinct states (capped at 4). An identical
 * candidate is skipped until the head has held for that gap (at least 2) and
 * the capture's VBL is a whole number of gaps from the newest VBL on which
 * the game drew (the head's own VBL before any drew), then committed as a
 * duplicate, so a held screen pairs a state with itself, the bracket stays
 * installed, and the duplicate's stamp is the stamp of the VBL whose drawn
 * frame the canonical present shows. Under deterministic capture @p tick_ns
 * is replaced by @p tick times half the period, raised to one period past
 * the head when that does not exceed the head's stamp. No-op when disabled
 * or when the region table is empty.
 * @param tick VBL counter of the capture (openpete_frame).
 * @param tick_ns Present-clock stamp of the capture.
 * @param drew Nonzero when the game run of this VBL drew a frame that owes a present. */
void openpete_interp_capture(uint32_t tick, op_time_t tick_ns, int drew) OP_REQUIRES(sim);

/** @brief Set the virtual present clock that sub-tick alpha is computed against.
 *
 * The run loop sets, before a present's re-extract, the instant that
 * present is displayed at: the output of a phase-locked loop over the
 * present stream, the eager producer's display instant minus its lag, or a
 * forced sub-tick point for the intermediate screenshot and the identity
 * audit. Presents are generated at uneven wall offsets but displayed at
 * even refresh slots; a phase taken from generation time reproduces a 3:1
 * velocity judder on the display. Callers restore 0 after the re-extract.
 * @param t Display instant on the op_now() clock, or 0 to fall back to
 *          op_now(). */
void      openpete_interp_set_present_target(op_time_t t);
/** @brief Return the present target, or op_now() when none is set.
 *
 * Every sub-tick alpha reads this clock, never op_now() directly.
 * @return The display instant of the present being produced. */
op_time_t openpete_interp_present_now(void);
/** @brief Return the present target as last set, 0 when none is set.
 *
 * Lets a caller that overrides the target for a span restore the value it
 * found.
 * @return The forced display instant, or 0. */
op_time_t openpete_interp_present_target(void);

/** @brief Arm or disarm deterministic capture stamps.
 *
 * Armed by run.c for intermediate screenshots. While armed,
 * openpete_interp_capture ignores the stamp it is passed and stamps each
 * commit with its VBL counter times P/2, P = @p period_ns (one tick, two
 * VBLs), so stamps strictly increase and depend on the VBL alone. A caller
 * that sets the present target to curr_tick_ns + P/2 then gets alpha 0.5 on
 * a pair one logic step apart, with no dependence on host pacing.
 * @param on Nonzero arms.
 * @param period_ns Grid spacing, stored whether or not @p on is set. */
void      openpete_interp_set_deterministic(int on, op_time_t period_ns);
/** @brief Report whether deterministic capture is armed.
 *
 * Sub-tick consumers that keep their own stamps (the native 2D overlay's
 * animation alpha, the player record store) switch to the tick grid while
 * it is armed so captures reproduce.
 * @return 1 when armed, else 0. */
int       openpete_interp_deterministic(void);
/** @brief Return the period last passed to openpete_interp_set_deterministic.
 *
 * @return The grid spacing in ns; 0 when never set. */
op_time_t openpete_interp_det_period(void);

/** @brief Return the newest stored snapshot's stamp.
 *
 * Under deterministic capture the stamp is the VBL clock; adding half the
 * period targets the sub-tick midpoint of a one-step pair.
 * @return curr->tick_ns, or 0 before init or while the head slot is
 *         invalid. */
op_time_t openpete_interp_curr_tick_ns(void);

/** @brief The deterministic-capture stamp of VBL @p tick for the per-object snapshot table.
 *
 * The VBL clock, @p tick times half the period, raised to the newest stored
 * snapshot's stamp when that is later. On a VBL whose region capture
 * committed, this is that commit's stamp; on a VBL the region ring skipped (no
 * registered region changed) it still advances, so an object that changes on
 * that VBL is stamped after the capture that last saw its previous state.
 * @param tick The VBL counter of the capture (openpete_frame).
 * @return The stamp in nanoseconds. */
op_time_t openpete_interp_det_vbl_stamp(uint32_t tick);
/** @brief Return the newest stored snapshot.
 *
 * Read-only. The player record pairing (games/common/renderer/player.c)
 * compares the bytes it captured at the render hook against the ring's
 * tick-end bytes of the same region.
 * @return The head snapshot, or NULL before init or until the ring holds
 *         one. */
const openpete_interp_snapshot_t* openpete_interp_curr_snapshot(void);

/** @brief Report whether the newest stored snapshot was committed as a duplicate of the one before it.
 *
 * A duplicate is committed when the registered regions have held for the
 * logic's cadence (openpete_interp_capture); its tick is the commit's VBL,
 * not the VBL on which the state first appeared.
 * @return 1 when the head is a duplicate commit, else 0 (also before init). */
int       openpete_interp_curr_is_dup(void);

/** @brief Report whether a sub-tick interpolation bracket is available.
 *
 * The endpoints may be byte-identical: a screen whose registered regions
 * are held still (the debug freecam) pairs the held state with itself, and
 * the bracket interpolates it as the identity.
 * @return 1 when enabled and the two newest slots are valid with equal byte
 *         counts; 0 otherwise, including the first captures after a reset. */
int       openpete_interp_paired(void);

/** @brief Report whether the ring's head is older than the latest capture by more than the pair's cadence.
 *
 * A screen whose registered regions stop changing stops rolling the ring:
 * the head stays at the tick the state last changed and the pair keeps
 * describing the last motion, while the head is the state on screen. Under
 * wall-clock stamps the growing present-minus-head distance clamps alpha to
 * 1 by itself. A consumer that phases a present onto the head's stamp (the
 * intermediate screenshot leg, the reextract and cut-guard alpha paths)
 * takes alpha = 1 when this returns 1, or it re-lerps the middle of a
 * motion that has ended. The tolerance is the stored pair's tick distance
 * (1 when prev is invalid): the head is legitimately that far behind the
 * latest capture while the logic advances at its own cadence.
 * @return 1 when stale; 0 when disabled, before any capture, with no valid
 *         head, or when the head is within the cadence. */
int       openpete_interp_curr_is_stale(void);

/** @brief Return the two newest snapshots and the alpha of @p now_ns between them.
 *
 * alpha = (@p now_ns - prev->tick_ns) / @p sim_period_ns, clamped to [0, 1]:
 * the present shows the state at prev->tick_ns + alpha * period, one tick
 * behind the newest capture, so the query never extrapolates. The endpoints
 * may be byte-identical (a held screen), in which case the lerp across them
 * is the identity; the ring, not the query, decides when such a pair is
 * stored. The out pointers are written only on success; each may be NULL.
 * @param now_ns Present instant, normally openpete_interp_present_now().
 * @param sim_period_ns Tick period; a non-positive value fails the query.
 * @param out_alpha Receives alpha.
 * @param out_prev Receives the earlier stored snapshot.
 * @param out_curr Receives the newest snapshot.
 * @return 1 when enabled and both slots are valid with equal byte counts,
 *         else 0. */
int openpete_interp_query(op_time_t now_ns,
                          op_time_t sim_period_ns,
                          double*   out_alpha,
                          const openpete_interp_snapshot_t** out_prev,
                          const openpete_interp_snapshot_t** out_curr);

/** @brief Run openpete_interp_query and also return the snapshot before prev.
 *
 * prev2 serves a cubic (Hermite) interpolant with a centred start tangent.
 * Called by each game's reextract.c and by the cut guard with
 * openpete_interp_present_now().
 * @param now_ns Present instant.
 * @param sim_period_ns Tick period.
 * @param out_alpha Receives alpha, as openpete_interp_query.
 * @param out_prev2 Receives the third-newest snapshot, or NULL when that slot
 *        is invalid or the query failed; callers then fall back to the linear
 *        prev-to-curr path. May be NULL.
 * @param out_prev Receives the earlier stored snapshot.
 * @param out_curr Receives the newest snapshot.
 * @return The openpete_interp_query result. */
int openpete_interp_query3(op_time_t now_ns,
                           op_time_t sim_period_ns,
                           double*   out_alpha,
                           const openpete_interp_snapshot_t** out_prev2,
                           const openpete_interp_snapshot_t** out_prev,
                           const openpete_interp_snapshot_t** out_curr);

/** @brief Re-read the ra_commit regions from committed guest RAM into the newest slot.
 *
 * The runahead driver in run.c calls this after rolling guest RAM back to
 * the committed tick; the ring was captured at the led speculation head.
 * Flagged regions then pair committed(T-1) with committed(T), the timeline
 * runahead-off produces, so they never show a misprediction. The slot is
 * overwritten in place even when this tick's capture deduped, and its
 * tick and tick_ns are kept. An unmapped region is left as it was. No-op
 * unless openpete_interp_ra_commit_active() or while the head is invalid. */
void openpete_interp_recapture_committed(void);

/** @brief Report whether the committed-region recapture is live.
 *
 * The spyro-1 re-extract uses this to skip the input-edge correction rebase
 * for regions that are committed-timeline by construction.
 * @return 1 when initialised, enabled, at least one region carries
 *         ra_commit and OPENPETE_RA_CAM_COMMIT is not 0 (read once);
 *         else 0. */
int openpete_interp_ra_commit_active(void);

/** @brief Return the number of candidates openpete_interp_capture has built.
 *
 * Counts every capture that read the regions, committed or deduped.
 * @return Candidate count since start. */
uint64_t openpete_interp_total_captures(void);
/** @brief Return the number of candidates openpete_interp_capture skipped as duplicates.
 *
 * @return Skipped-candidate count since start. */
uint64_t openpete_interp_total_dedupe_skips(void);

/** @brief Report the interpolation toggle: 1 runs the native sub-tick intermediate presents, 0 presents canonical ticks only.
 *
 * The toggle is the `interp` registry row and the interp_toggle key; it
 * starts on. It is one of the terms of host_subtick_live, so it changes
 * which presents are made and never a tick.
 * @return 1 when on, else 0. */
int openpete_interp_toggle_on(void);

/** @brief Set the interpolation toggle and log the change.
 *
 * Callable from any thread: the host loop reads the toggle once per
 * iteration, so a write takes effect from the next one.
 * @param on Nonzero turns sub-tick presents on. */
void openpete_interp_toggle_set(int on);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_INTERP_H */
