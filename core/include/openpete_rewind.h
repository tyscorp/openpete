/* core/include/openpete_rewind.h - rewind keyframe ring.
 *
 * A ring of in-memory snapshots (openpete_state_mem_t) captured every
 * OPENPETE_REWIND_KF_VBLS while gameplay is live, paired with the
 * input_log index at capture so the driver can replay forward from a
 * keyframe with the exact recorded inputs:
 *
 *   invariant: at capture, input_log entry (log_idx - 1) is the input
 *   for VBL `frame`; entry (log_idx + k) is the input for frame+1+k.
 *
 * The DRIVER lives in run.c (it needs the game fiber + pad overrides):
 * hold Backspace -> pick(target, park site) -> quiet mem-load -> stamp inputs from
 * the log per VBL -> co_switch to target -> truncate log + drop_after.
 *
 * Scope: same level only. Keyframes are cleared whenever gameplay
 * liveness drops (level transition/loading - the mem snapshot carries
 * VRAM but not SPU sample RAM, which a load rewrites) and on any real
 * savestate load (via the
 * discontinuity broadcast - a loaded state is a different timeline).
 * Memory: ~3 MiB per keyframe (2 MiB RAM, 1 MiB VRAM, scratchpad, SPU
 * voice state, CD queue), one
 * keyframe per second of window plus one, plus the mod arena's pages:
 * each keyframe's arena is a table of reference-counted 4 KiB pages
 * (openpete_rewind_pages.h), copied whole at the keyframe and then
 * compared, a slice per call over the following ticks, against the newest
 * other valid keyframe, whose equal pages it shares. */

#ifndef OPENPETE_REWIND_H
#define OPENPETE_REWIND_H

#include "openpete_state.h"
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

#define OPENPETE_REWIND_KF_VBLS 60u   /* one keyframe per second */

typedef struct {
    openpete_state_mem_t* mem;
    unsigned              frame;    /* openpete_frame at capture */
    int                   log_idx;  /* input_log_count() at capture */
    int                   valid;
} openpete_rewind_kf_t;

/** @brief Size the keyframe ring for a rewind window, freeing the previous ring, its snapshots and the arena page store.
 *
 * The ring holds window_seconds + 1 keyframes, each snapshot allocated on
 * first capture. The first enabling call registers a guest-discontinuity
 * handler that clears the ring on a savestate load. Called on the host loop's
 * thread only, which also writes the snapshots: at startup from --rewind and
 * from the live-knob drain for openpete_rewind_window_set. An allocation
 * failure leaves rewind disabled.
 * @param window_seconds Window length; 0 or less disables rewind. */
void openpete_rewind_configure(int window_seconds);
/** @brief Report whether a rewind ring is allocated.
 *
 * @return Nonzero after a successful openpete_rewind_configure with a
 *         positive window, 0 otherwise. */
int  openpete_rewind_enabled(void);

/** @brief Capture a keyframe when one is due, else run one slice of the arena compare; clear the ring when gameplay is not live.
 *
 * A keyframe is due on the first call after a clear and then every
 * OPENPETE_REWIND_KF_VBLS VBLs. It first finishes the previous keyframe's
 * compare, releases the slot it overwrites, copies the snapshot and the
 * arena's used pages, records input_log_count(), and starts the compare
 * against the newest other valid keyframe. A failed snapshot allocation
 * or save skips the capture and retries on the next call; a page store
 * that cannot grow drops the keyframe (counted) and the next one is due a
 * full interval later. Between keyframes each call compares the pages left
 * divided by the calls left before the next keyframe (one call per tick,
 * two VBLs). Called once per host iteration at the savestate quiescent
 * point, with the game fiber yielded. No effect when rewind is disabled.
 * @param cpu Guest CPU state to snapshot.
 * @param frame Current openpete_frame (VBL count).
 * @param live Non-zero while gameplay is live; 0 during a level
 *        transition or load, whose mutated sections the snapshot
 *        excludes. */
void openpete_rewind_on_iter(const CPUState* cpu, unsigned frame, int live);

/** @brief Find the newest valid keyframe at or before a VBL, optionally only among those saved at one park site.
 *
 * A keyframe loads only while the game fiber is parked at the park site it
 * was saved at, so the driver asks for the current site's.
 * @param target openpete_frame value to rewind to.
 * @param site The park site the keyframe must carry (a keyframe with no
 *        site matches any), or NULL for any site.
 * @return The keyframe with the largest frame not above @p target, or
 *         NULL when none is valid there. */
const openpete_rewind_kf_t* openpete_rewind_pick(unsigned target, const openpete_park_site_t* site);

/** @brief Restore a keyframe from openpete_rewind_pick: the snapshot quietly, then its arena pages.
 *
 * A keyframe whose arena compare is still running restores from its
 * partly compared table, which names every page.
 * @param kf A valid keyframe of the ring.
 * @param cpu Guest CPU state whose register prefix is overwritten.
 * @param out_frame Receives the keyframe's frame; may be NULL.
 * @return openpete_state_mem_load's result (0 restored, 1 park site
 *         mismatch, -1 refused, -2 partial restore); -1 for a keyframe not
 *         in the ring. */
int  openpete_rewind_restore(const openpete_rewind_kf_t* kf, CPUState* cpu, unsigned* out_frame) OP_REQUIRES(sim);

/** @brief Report whether a --rewind-hold may start at this VBL under OPENPETE_REWIND_HOLD_PHASE.
 *
 * Harness: with OPENPETE_REWIND_HOLD_PHASE=n set, a hold starts only at
 * the VBL n after the newest keyframe (0: the keyframe's own iteration,
 * which restores fully compared keyframes; about 10: a keyframe whose
 * arena compare is still running). Unset, always 1.
 * @param frame Current openpete_frame.
 * @return 1 when the hold may start. */
int  openpete_rewind_hold_phase_ready(unsigned frame);

/** @brief Log the arena page counters, the keyframes' arena memory and the keyframe-copy and compare-slice costs (median, p99, max); silent when no keyframe was taken. */
void openpete_rewind_report(void);

/** @brief Invalidate every keyframe captured after a VBL.
 *
 * Called after a rewind lands, since those keyframes describe the
 * discarded future. Their snapshots stay allocated for reuse; their arena
 * pages are released.
 * @param frame openpete_frame the rewind landed on. */
void openpete_rewind_drop_after(unsigned frame);

/** @brief Invalidate every keyframe and make the next call to openpete_rewind_on_iter capture one.
 *
 * Snapshots stay allocated; arena pages are released to the page store,
 * which keeps its slabs. Called on liveness loss, on a savestate load
 * through the discontinuity handler, on a mod-set change, and by
 * openpete_rewind_configure. */
void openpete_rewind_clear(void);

/** @brief Record a host intervention in guest state, so the next rewind step clears the rewind history.
 *
 * A rewind restores a keyframe and replays the ticks after it from the
 * logged pad input only, so a teleport, a warp, a cheat, a free camera pose
 * or a free camera engage or disengage applied inside the window would be
 * missing from the rebuilt state. Callable from any thread: it bumps an
 * epoch that the host loop's rewind step (host_state_ops.c) compares before
 * its keyframe capture, so no keyframe older than the intervention survives
 * to be restored.
 * @param what What was applied, a string literal the clear's log line names. */
void openpete_rewind_note_intervention(const char* what);

/** @brief Read the intervention epoch openpete_rewind_note_intervention bumps.
 *
 * @param what Receives the last intervention's name; may be NULL.
 * @return The number of interventions recorded since start. */
unsigned openpete_rewind_intervention_epoch(const char** what);

/** @brief Report whether the canonical world draw running now belongs to a replayed tick whose scene the host does not present.
 *
 * Called once per canonical world draw, through openpete_draw_unseen
 * (openpete_fiber.h). While a rewind replays ticks
 * toward its target frame, a draw is predicted to be the replay's last,
 * whose scene the host presents, when the frame plus the interval between
 * the two replayed draws before it (2 VBLs before there are two) reaches
 * the target; every earlier draw is unseen. Outside a replay it returns 0.
 * The rewind driver counts a replay whose last draw was predicted unseen.
 * @return 1 for an unseen draw, else 0. */
int openpete_rewind_draw_unseen(void) OP_REQUIRES(sim);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_REWIND_H */
