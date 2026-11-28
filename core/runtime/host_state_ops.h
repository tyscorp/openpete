// core/runtime/host_state_ops.h - the savestate request queue, the stress
// harnesses and the rewind driver the host loop runs at final yields.
#pragma once

#include "openpete.h"
#include "openpete_thread_roles.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief Attribute one deferred request to the emitted polling loop whose delivery the fiber is yielding under, if any. */
void count_loop_deferral(void);

/** @brief Run every queued savestate request at a final yield, or keep the queue and count the wait.
 *
 * Requests run in arrival order. A load whose state was saved at another
 * park site stops the drain there: it and every later request stay queued
 * for the next final yield (state_op_site_wait). A slot request toasts when
 * it finishes or is refused, never while it waits (state_op_toast).
 * @return 1 at a final yield (the queue ran and the periodic state work may run), else 0. */
int state_ops_drain(void) OP_REQUIRES(sim);

/** @brief Queue the savestate requests raised this iteration, then apply the slot-cycle key and announce a slot change.
 *
 * Requests are the hotkeys, the dev overlay's Save and Load, and the
 * scheduled one-shots; the slot ones use the active slot as it stood before
 * this iteration's cycle. The scheduled save is queued before the scheduled
 * load, so a save and a load armed for the same tick capture and then
 * restore (a same-tick roundtrip).
 * @param opts Run options (the --state-*-on-frame one-shots). */
void state_ops_collect(const openpete_options_t* opts) OP_REQUIRES(sim);

/** @brief Step the savestate stress harnesses (restore-at-yield checks) by one quiescent iteration.
 * @param opts Run options (--state-stress, --state-stress-rewind). */
void state_stress_step(const openpete_options_t* opts) OP_REQUIRES(sim);

/** @brief Run one quiescent iteration of rewind: the keyframe cadence, then one tick back while the rewind key or the rewind_hold row holds it in live, unpaused gameplay.
 * @param opts Run options.
 * @param paused The host is paused.
 * @param bk2_active A movie is loaded for replay.
 * @return true when the timeline moved (the iteration skips its forward tick). */
bool rewind_step(const openpete_options_t* opts, bool paused, bool bk2_active) OP_REQUIRES(sim);

/** @brief Run the state half of a quiescent iteration: queue this iteration's savestate requests, then at a final yield run the queue, the stress harnesses, the rewind driver, the mod arena page census, the process handoff and the bug-report recorder step.
 *
 * The game fiber is yielded, so g_game.cpu and g_ram are quiescent and can
 * be snapshotted or restored without racing the recompiled code. The
 * handoff resumes on the first pass of a --resume-handoff process and
 * checkpoints and execs when armed (OPENPETE_HANDOFF_AT,
 * openpete_handoff_request).
 * @param opts Run options.
 * @param paused The host is paused.
 * @param bk2_active A movie is loaded for replay.
 * @return true when the rewind driver moved the timeline (the iteration skips its forward tick). */
bool state_ops_quiescent(const openpete_options_t* opts, bool paused, bool bk2_active) OP_REQUIRES(sim);

/* A run of guest ticks the host executes in one iteration of which it
 * presents only the scene of the last canonical world draw (a rewind
 * replay, a runahead lead): each draw's unseen verdict and the counts its
 * mispredictions feed. */
#define DRAW_RUN_NONE 0xFFFFFFFFu
typedef struct {
    unsigned target;    /* the frame the run's yields end at */
    unsigned draw_at;   /* the frame of the run's last draw, DRAW_RUN_NONE before one */
    unsigned period;    /* VBLs between the run's last two draws, 2 before there are two */
    int last_unseen;    /* the verdict of the last draw since the last close */
    int drawn;          /* a draw was noted since the last close */
    unsigned seen;      /* draws predicted seen since the last close */
} draw_run_t;

/** @brief Aim a draw run at the frame its yields end at and forget its draw frames.
 * @param r The run.
 * @param target The frame the run ends at. */
void draw_run_aim(draw_run_t* r, unsigned target);

/** @brief Predict from the frame alone whether the draw at @p frame is not the run's last: the frame plus the interval between the run's two previous draws (2 VBLs before there are two) falls short of the target.
 * @param r The run; its draw frame and interval advance.
 * @param frame The draw's frame.
 * @return 1 when the draw is predicted unseen, else 0. */
int draw_run_frame_unseen(draw_run_t* r, unsigned frame);

/** @brief Record a draw's verdict in the run.
 * @param r The run.
 * @param unseen The draw's verdict.
 * @return @p unseen. */
int draw_run_note(draw_run_t* r, int unseen);

/** @brief Close the run's presented scene: when a draw was noted, count a last draw predicted unseen and the draws predicted seen that a later draw followed, then clear the verdicts.
 * @param r The run.
 * @param unseen_last Incremented when the presented draw was predicted unseen.
 * @param seen_early Increased by the draws predicted seen before the last.
 * @return 1 when the presented draw was predicted unseen, else 0. */
int draw_run_close(draw_run_t* r, unsigned long long* unseen_last,
                    unsigned long long* seen_early);

/** @brief Log the savestate census: requests deferred to a final yield, runahead deferrals and adoptions, park-site deferrals, the first emitted-loop-site deferral and the bug-report recorder's report. */
void state_ops_report(void) OP_REQUIRES(sim);

/* Park-site deferrals by kind: a load whose snapshot was saved at another
 * park site than the current final yield's. */
enum { SITE_DEF_LOAD, SITE_DEF_REWIND, SITE_DEF_STRESS, SITE_DEF_RUNAHEAD, SITE_DEF_N };
extern unsigned long long s_site_deferred[SITE_DEF_N];
extern unsigned long long s_site_refused;
extern unsigned long long s_site_wait_yields;
extern unsigned long long s_site_abandoned;
extern unsigned long long s_state_ops_deferred[2];
extern unsigned long long s_state_yields_skipped;
extern unsigned long long s_loop_deferred;
extern uint32_t s_loop_deferred_header;
extern unsigned s_loop_deferred_key;
