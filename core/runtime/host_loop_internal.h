// core/runtime/host_loop_internal.h - the host loop's session: what one run
// opens before its first iteration and closes after its last.
#pragma once

#include "openpete.h"
#include "host_pacer.h"
#include "openpete_frame_time_log.h"
#include "bk2_movie.h"
#include "openpete_thread_roles.h"
#include "openpete_time.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* The bk2 movie session: replay (--bk2) and recording (--bk2-record). */
typedef struct {
    bk2_movie_t movie;      /* owner: open (bk2 load); freed by close */
    int         offset;     /* owner: open (bk2 load); the movie row of feed key 0 */
    bool        active;     /* owner: open; a movie is loaded for replay */
    bool        recording;  /* owner: open; bk2_record_begin succeeded */
} host_bk2_t;

/* The run's output files. The pace log and the profile are module state
 * (openpete_pace_log_*, op_prof_*); their close is a no-op when unopened. */
typedef struct {
    FILE* position_log;                /* owner: open; --position-log, else NULL */
    FILE* hash_log;                    /* owner: open; --gameplay-hash-log, else NULL */
    openpete_frame_time_log_t ftl;     /* owner: open; appended by the inline and eager presents */
} host_outputs_t;

/* One run of the host loop. Zeroed by host_session_open; every field a
 * failed open leaves set records a resource host_session_close releases. */
typedef struct host_session {
    const openpete_options_t* opts;    /* owner: open */
    bool        split;                 /* owner: open; the thread split applies */
    Uint64      start;                 /* owner: open; SDL ticks the run's elapsed time counts from */
    const char* exit_reason;           /* owner: open on a setup error, else host_exit_reason; the loop sets "bk2-end" when host_input_arbitrate ends the movie; NULL while the run continues */
    int         exit_code;             /* owner: close (its argument); raised by the psxtrace verdict */
    bool        loop_entered;          /* owner: the loop; close runs its output steps only when set */
    int         refresh_hz;            /* owner: open; display refresh, 60 headless or unknown */
    int         refresh_mhz;           /* owner: open; display refresh in mHz, for the pace log */
    bool        paused;                /* owner: host_pause_step; the sim is paused */
    bool        step_one;              /* owner: host_pause_step (raised while paused, lowered on resume); lowered by host_tick when the step ends */
    bool        ram_dumped;            /* owner: host_ram_dump_if_due; the --ram-dump-on-frame dump was taken */
    op_time_t   last_canon_present_ns; /* owner: host_present (the canonical present); 0 until the first, read by host_world_live */
    host_pacer_t   pacer;              /* owner: open (init, start); live changes applied by host_iter_begin, advanced by host_pace_tail */
    host_bk2_t     bk2;                /* owner: open (bk2 load, record begin) */
    host_outputs_t out;                /* owner: open; closed by close */
} host_session_t;

/* The pace-log record of the VBL an iteration runs: the input sample it
 * latched, the deadline in force, the flags and the execution bracket. */
typedef struct {
    op_time_t latch_ns;    /* owner: host_input_latch; publish instant of the latched sample, 0 on the legacy path */
    unsigned  latch_seq;   /* owner: host_input_latch; the latched sample's identity, OP_PACE_LATCH_NONE on the legacy path */
    uint16_t  latch_bits;  /* owner: host_input_latch; pad bits of the latched sample */
    op_time_t deadline_ns; /* owner: host_tick; the pacer deadline in force, 0 when unpaced */
    uint8_t   flags;       /* owner: host_tick (deadline flags, SPECULATIVE); appended by host_pace_tail (PAUSED, STEPPED, REWIND, NOT_LIVE) */
    op_time_t exec_start_ns; /* owner: host_tick; the game fiber switch's start, 0 when the iteration does not yield */
    op_time_t exec_end_ns;   /* owner: host_tick; the game fiber switch's end, 0 when the iteration does not yield */
} host_vbl_rec_t;

/* One iteration of the host loop. Zeroed by host_iter_begin. */
typedef struct {
    bool unlock_at_entry;   /* owner: host_iter_begin; the unlock verdict from the previous keymap sample */
    bool sim_due;           /* owner: host_iter_begin, after the live pacer changes; the sim runs this iteration */
    op_time_t sim_period_ns; /* owner: host_iter_begin; the VBL period the world-liveness bound and the VBL clock read */
    bool ovr_claimed;       /* owner: host_input_arbitrate; a bk2 row or an overlay claimed the pad override channel */
    unsigned bk2_stamped_frame; /* owner: host_input_arbitrate; openpete_frame the bk2 stamp was made for */
    int      bk2_stamped_row;   /* owner: host_input_arbitrate; the bk2 row it stamped (bk2_feed_row), when sim_due */
    bool p1_staged;         /* owner: host_input_latch; slot 0 of the input record was staged before the yield */
    bool rewound;           /* owner: host_quiescent_ops (rewind_step); the rewind driver stepped, so no forward tick */
    unsigned frame_before_yield; /* owner: host_tick, before the yield; read by host_input_commit */
    bool yield_now;         /* owner: host_tick; the game fiber runs this iteration */
    bool ticked;            /* owner: host_tick; the yield advanced openpete_frame */
    bool did_present;       /* owner: host_present (the canonical present); a drawn tick was presented this iteration */
    host_vbl_rec_t vbl;     /* owner: host_input_latch (latch fields), host_tick (the rest) */
} host_iter_t;

/** @brief Run the host loop on the sim thread: open the session, run iterations until an exit reason is set, close the session.
 *
 * Run apart from run_host_init because SDL brings its video, gamepad and
 * haptic subsystems up on one thread and pumps them there: init runs on
 * main before main starts pumping, which serialises the two with no
 * barrier. The game fiber is created and switched here, on the one thread
 * that owns it for its whole life.
 *
 * The loop body is the phase sequence, one call per phase. The order is
 * the contract:
 * - The live pacer changes run before sim_due, since a regrid moves the
 *   deadline sim_due reads; the unlock peek reads the previous iteration's
 *   keymap sample.
 * - The bk2 row is stamped on a sim-due iteration only, the overlays'
 *   neutral stamp on every iteration; the movie's end leaves before the
 *   tick of a row past it.
 * - The keymap poll runs before the first hotkey read.
 * - The quiescent ops run before the input latch, because a savestate load
 *   rewrites the pad buffer; the input is pumped and latched on every
 *   iteration, since the pump propagates the override into the pad buffer
 *   the game polls, and the input record is staged after every override
 *   writer.
 * - The tick runs the game fiber only on a latch decision made this
 *   iteration; the interp captures are taken at the runahead lead's head,
 *   and the rollback precedes every other RAM read.
 * - The tick outputs describe the committed timeline (after the rollback).
 * - The present never runs inline while the present thread holds the
 *   display.
 * - The unlock verdict follows the present, which consumed the previous
 *   iteration's swap-interval flag; the pacer runs last.
 *
 * No scene is begun between iterations: DrawOTag starts the scene on demand.
 * @param opts The parsed options.
 * @return The process exit code. */
int run_host_loop(const openpete_options_t* opts) OP_REQUIRES(sim);

/** @brief Open the run: display claim, mods guard, game boot hook, game fiber, deadline, refresh query, pacer, rewind ring, runahead request and the outputs.
 *
 * Zeroes @p S first. On a failure S->exit_reason names the step and every
 * resource opened so far stays recorded in @p S for host_session_close.
 * @param S The session.
 * @param opts The parsed options.
 * @return OPENPETE_EXIT_OK, OPENPETE_EXIT_PANIC when the game fiber cannot be allocated, or OPENPETE_EXIT_SETUP_ERROR when an output cannot be opened. */
int host_session_open(host_session_t* S, const openpete_options_t* opts) OP_REQUIRES(sim);

/** @brief Tear the run down in order and return its exit code.
 *
 * Each step is a release step, which runs on every path and is guarded by
 * what @p S records as opened, or an output step (reports, the psxtrace
 * verdict, the bk2 movie write), which runs only when S->loop_entered.
 * On a setup error the bk2 recorder is discarded instead of finalized, so
 * no zero-row movie replaces the file at the record path.
 * @param S The session.
 * @param rc The exit code so far: the loop's, or the failed open step's.
 * @return @p rc, or OPENPETE_EXIT_DIVERGENCE when the psxtrace verdict failed a run that was OK. */
int host_session_close(host_session_t* S, int rc) OP_REQUIRES(sim);

/** @brief Build the pacing environment block (host_pace_env_build) of the open session as it stands now.
 * @param env Receives the block; "" with no session open.
 * @param env_bytes Capacity of @p env. */
void host_session_pace_env(char* env, unsigned env_bytes) OP_REQUIRES(sim);

/** @brief Report whether the open session's sim is paused.
 * @return false with no session open. */
bool host_session_paused(void) OP_REQUIRES(sim);

/** @brief Log the session's sim-thread subsystem reports without changing their state: the pacing census, slip totals, interpolation, runahead, stream, capture, disc, input, rumble, rewind, savestate, VBL delivery, OSD, version and GPU-audit lines.
 *
 * The bug-report press calls it under a log capture, so the lines land in
 * the bundle; every report here only reads its counters. */
void host_session_press_reports(void) OP_REQUIRES(sim);

/** @brief Log the display owner's reports without changing their state: the swapchain line, the native present and eager censuses and the present thread's report.
 *
 * Called while the present thread's device is borrowed, so its counters
 * are quiescent. */
void host_session_display_reports(void) OP_REQUIRES(sim);

/** @brief Run the VBL an iteration owes: the yield decision, the game fiber switch inside the pace-log bracket, then on a tick the game hook, the runahead lead, the interp captures and the runahead settle.
 *
 * Consumes the pacer's VBL deadline and its flags every iteration, whether
 * or not it yields. The game fiber runs when the sim is due, not paused
 * (or a paused step is pending) and the rewind driver did not step. Leaves
 * RAM at the committed tick.
 * @param S The session: the pacer, and the pause and step flags.
 * @param it The iteration: reads sim_due, rewound, sim_period_ns and the
 *        latch fields; sets frame_before_yield, yield_now, ticked and the
 *        rest of vbl. */
void host_tick(host_session_t* S, host_iter_t* it) OP_REQUIRES(sim);

/** @brief Run the quiescent ops of an iteration: the state half (savestate queue, stress, rewind, handoff), the input restamp, pause and step, then the host hotkeys.
 *
 * The game fiber is yielded, so a savestate op can snapshot or restore
 * without racing the recompiled code, and it runs before the input latch so
 * a load that overwrites the pad buffer is followed by a fresh write of
 * current input. While the settings menu is open every op runs and each
 * site's key read is masked (host_control.h, HC_GATE_MENU), turbo's
 * excepted; the keymap latch keeps advancing underneath, so a press made
 * under the menu is swallowed rather than replayed.
 * @param S The session: the options, the pause flags, the bk2 session and the split.
 * @param it The iteration: sets rewound. */
void host_quiescent_ops(host_session_t* S, host_iter_t* it) OP_REQUIRES(sim);

/** @brief Write the --ram-dump-on-frame RAM image, and the CPU register block when --halt-cpu-dump-output is set, once, on the first iteration whose feed key reached the target.
 * @param S The session: the options and the one-shot flag. */
void host_ram_dump_if_due(host_session_t* S) OP_REQUIRES(sim);

/** @brief Return the reason the run ends after this iteration, or NULL to continue.
 *
 * Precedence: the psxtrace verdict is observed first, so its log line is
 * emitted; a requested shutdown then overrides every reason ("user-quit");
 * then the psxtrace reason, the game returning, --frame-limit,
 * --record-frames and --max-runtime-seconds.
 * @param S The session: the options and the run start.
 * @return The exit reason, or NULL. */
const char* host_exit_reason(const host_session_t* S) OP_REQUIRES(sim);

/** @brief On an iteration the sim was due, log the loop sample every 64th openpete_frame and clear openpete_primitives_drawn.
 * @param S The session: the run start.
 * @param it The iteration: reads sim_due. */
void host_loop_sample(const host_session_t* S, const host_iter_t* it) OP_REQUIRES(sim);

/** @brief Run the present phase: the held-present gate, the canonical present of a drawn tick, then the eager or inline strategy between ticks and the paused repaint.
 *
 * The canonical present runs when the game owes one and no skip
 * fast-forward holds it. Eager production disengages here on any iteration
 * whose sub-tick presentation is no longer live, so the inline path never
 * presents while the present thread holds the display.
 * @param S The session: the options, the pacer, the pause flags, the outputs and the world-liveness stamp.
 * @param it The iteration: reads sim_due and sim_period_ns; sets did_present. */
void host_present(host_session_t* S, host_iter_t* it) OP_REQUIRES(sim);

/** @brief Report whether the world is live: a canonical present was made within six VBLs of now.
 *
 * During a CD load or level transition the game fiber stops rendering
 * while the clock keeps running; a re-extract then would read RAM the
 * incoming level is overwriting, so sub-tick presents hold the last
 * composed scene instead. False until the first canonical present.
 * @param S The session: the world-liveness stamp.
 * @param it The iteration: the VBL period, scaled by --tick-scale.
 * @param now The instant judged.
 * @return true when canonical presents are flowing. */
bool host_world_live(const host_session_t* S, const host_iter_t* it, op_time_t now);

/** @brief Record a shown present: the profiler's PRESENT record, then the frame-time-log record.
 *
 * Called on the sim thread for an inline present and on the present thread
 * for each present it makes; the profiler record carries
 * openpete_canon_tick and the frame-time-log record openpete_frame.
 * @param ftl The run's frame-time log.
 * @param kind OP_PACE_CANONICAL, OP_PACE_INTERMEDIATE or OP_PACE_REPEAT (equal to the OP_PROF_* kinds). */
void present_record_shown(openpete_frame_time_log_t* ftl, int kind);

/** @brief Log the eager-production totals, the count of sub-tick re-extracts presented, eager and inline, and the headless forced re-extract's row. */
void host_present_exit_report(void);

/** @brief Report whether OPENPETE_HEADLESS_REEXTRACT asks a headless run to re-extract at every non-drawing VBL.
 *
 * A harness variable: the oracle's re-extract row sets it, so the guest
 * render code the sub-tick view brackets runs against gameplay RAM in a
 * headless run. The pacer enables the deterministic interpolation ring
 * under it.
 * @return true when the variable is set to a value not starting with '0'. */
bool host_headless_reextract_on(void);
