// core/runtime/host_present.c - the host loop's present phase: the canonical
// present of a drawn tick, the eager or inline strategy that fills the
// display between ticks, the inline shed predictor, and the world-liveness
// stamp both strategies gate on.

#include "openpete_bugreport.h"
#include "openpete_bugrec_subtick.h"
#include "host_loop_internal.h"
#include "eager.h"
#include "host_pacer.h"
#include "host_hotkeys.h"
#include "run_internal.h"

#include "openpete_fb_snapshot.h"
#include "openpete_fiber.h"
#include "openpete_host_mouse.h"
#include "openpete_host_trace.h"
#include "openpete_ident_audit.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_log.h"
#include "openpete_mods.h"
#include "openpete_native_render.h"
#include "openpete_pace_log.h"
#include "openpete_present_thread.h"
#include "openpete_profile.h"
#include "openpete_record.h"
#include "openpete_run_capture.h"
#include "openpete_subtick_view.h"
#include "openpete_terrain_cache.h"
#include "openpete_ticker.h"
#include "openpete_time.h"
#include "../rhi/rhi_gpu_audit.h"
#include "bk2_feed.h"
#include "screenshot.h"

#include <stdlib.h>

#ifdef OPENPETE_HAS_PSYCROSS
#include "PsyX/PsyX_public.h"  /* PsyX_EndScene */

/* Player screenshot pending (the `screenshot` binding). Raised where the
 * bindings are sampled, consumed at the next canonical present's capture
 * site - both on this thread, so a plain int is enough. */
static int s_screenshot_request = 0;

/** @brief Advance the virtual present clock and pass it to openpete_interp_set_present_target as this present's instant.
 *
 * Off unless OPENPETE_PRESENT_TARGET_CLOCK is set to a value not starting
 * with '0'; while off the function returns at once and interpolation phase
 * comes from op_now(). While on, the clock steps by an EMA (weight 0.05) of
 * the measured present-to-present interval (samples under 100 ms), pulls
 * 1/16 of the remaining distance toward op_now(), and is clamped to within
 * 6 ms of op_now(); the first present, or a clock more than 50 ms from
 * op_now(), resyncs it to op_now(). Each present's content is then timed for
 * an evenly spaced display slot instead of the instant it was generated at,
 * which removes submit-time jitter. It also gives a present held on screen
 * through a tick the same slice of motion as one shown for one refresh,
 * which is why it is off by default. */
static void vp_set_present_target(void) {
    static int s_on = -1;
    if (s_on < 0) {
        /* Off by default: the clock spaces animation time evenly per present,
         * but display time is not evenly spaced. The tick executes on this
         * thread, so one image is held on screen for the length of it, and an
         * evenly spaced alpha gives that image the same slice of motion as an
         * image shown for one refresh: a systematic animation error each tick
         * (about 5 ms of animation at --render-fps 480) plus a small mid-tick
         * bias of the opposite sign. Letting the phase follow wall time
         * removes that term.
         *
         * What the clock buys is removal of submit-time jitter; without it
         * the error tail widens (p99 0.6-1.7 ms -> about 2.2 ms, roughly one
         * refresh at 480 Hz). A periodic deficit every tick is more visible
         * than random error of that size on a small share of presents, so the
         * systematic term is the one to remove. The [-6, +6] ms clamp below
         * keeps the clock inside the range the consumers accept, since they
         * allow slightly negative alpha rather than snapping shown time
         * forward at every interval roll. */
        const char* e = getenv("OPENPETE_PRESENT_TARGET_CLOCK");
        s_on = (e && *e && *e != '0');
    }
    if (!s_on) return;
    static op_time_t s_clk = 0, s_last = 0;
    static double s_sp = 0.0;
    op_time_t now = op_now();
    if (s_last != 0) {
        double d = (double)(now - s_last);
        if (d > 0.0 && d < 100.0 * (double)OP_NS_PER_MS) {
            if (s_sp <= 0.0) s_sp = d;
            else s_sp += (d - s_sp) * 0.05;
        }
    }
    s_last = now;
    if (s_clk == 0 || s_sp <= 0.0 ||
        now > s_clk + 50 * OP_NS_PER_MS || now + 50 * OP_NS_PER_MS < s_clk) {
        s_clk = now;                      /* first present / stall / jump: resync */
    } else {
        s_clk += (op_time_t)s_sp;         /* even step at the measured cadence */
        s_clk += (now - s_clk) / 16;      /* bounded pull toward reality */
        /* Hard drift bound (+-6 ms): the capture stamps live on their own
         * PLL-smoothed grid anchored to wall time; this clock smooths the
         * present cadence, so the two are independent oscillators. When the
         * present cadence shifts under load (runahead speculation inside a
         * tick iteration), the 1/16 pull is too weak and the standing offset
         * becomes an interp-alpha bias: presents clamp at the interval's
         * start and its tail is never shown, so motion arrives as 30 Hz
         * roll-jumps. Smoothing within +-6 ms keeps the jitter rejection;
         * beyond that the wall clock wins. */
        const op_time_t cap = 6 * OP_NS_PER_MS;
        if (s_clk < now - cap) s_clk = now - cap;
        else if (s_clk > now + cap) s_clk = now + cap;
    }
    openpete_interp_set_present_target(s_clk);
}

/** @brief Return 1 once per process when OPENPETE_GPU_AUDIT_SELFTEST asks to audit one real present.
 *
 * The caller sets g_openpete_guest_running around that native present, so
 * the GPU audit records its Vulkan calls. A reading of zero Vulkan calls
 * during guest execution is evidence only from a build shown to catch
 * calls; a mis-wired macro set reports the same zero as a clean tick.
 * @return 1 on the first call with the variable set to a value not starting
 *         with '0', else 0. */
static int gpu_audit_selftest_arm(void)
{
    static int state = -1;
    if (state < 0) {
        const char *e = getenv("OPENPETE_GPU_AUDIT_SELFTEST");
        state = (e && *e && *e != '0') ? 1 : 0;
    }
    if (state != 1) return 0;
    state = 0;          /* one present only */
    return 1;
}

/* The inline intermediate present's cost predictor. An intermediate present
 * is optional and a tick is not: a present predicted not to finish before
 * the sim's next deadline is shed. The estimate is an EMA of what
 * intermediate presents have taken on this machine in this scene, so it
 * adapts to the hardware instead of encoding a guess about it. */
typedef struct {
    op_time_t cost_ema;     /* rolling cost of an intermediate present */
    /* EMA of |cost - ema|: the WIDTH of the cost distribution, not just its
     * centre. The predicate needs an upper bound on what the next present will
     * cost, and an EMA alone is a centre estimate - on a right-skewed cost
     * distribution it is exceeded constantly. Measured over 49k recorded presents,
     * the shipped `ema + ema/4` bound was exceeded by 18-19 % of presents, in
     * the attract demo and in a level alike. */
    op_time_t cost_dev;
    int       n;            /* presents measured, up to PACE_COST_WARMUP */
    int       shed_run;     /* consecutive sheds since the last admission */
    /* Largest remaining-budget seen at an intermediate opportunity, decayed so it
     * tracks the machine instead of latching on one lucky moment. The probe
     * needs "the moment in the cycle with the most budget"; expressed as a FIXED
     * fraction of a tick that moment does not exist on every machine - measured on
     * an Arc 140V laptop, the budget at 11 119 consecutive sheds never once
     * exceeded 9.6 ms against a 16.7 ms requirement, so the probe could not fire
     * and interpolation stayed off for the whole run. */
    op_time_t budget_peak;
} shed_predictor_t;

/* The predictor will not act until it has measured this many presents, so a
 * cold start cannot shed on an estimate of zero. */
#define PACE_COST_WARMUP 16
/* Consecutive sheds before one present is admitted anyway, purely to
 * re-measure. Without it the estimator is one-way. */
#define PACE_SHED_PROBE_EVERY 32

/* The run's inline predictor, on the sim thread. Under eager production it
 * is not consulted and keeps what it last learned. */
static shed_predictor_t s_shed;

/* A predictor verdict on one intermediate present. */
typedef enum {
    SHED_ADMIT_STRICT,   /* the strict margin fits before the deadline */
    SHED_ADMIT_LATE,     /* the tail bound fits inside the lateness budget */
    SHED_PROBE,          /* neither fits; admitted to re-measure the cost */
    SHED_SHED,           /* neither fits; not presented */
} shed_verdict_t;

/* Sub-tick re-extracts driven by the inline intermediate present, the path
 * taken whenever eager production is not active. Reported beside eager's
 * own count so one exit line states how many presents in a run were built
 * from interpolated gamestate rather than from a tick. */
static unsigned s_inline_rex = 0;

/* OPENPETE_EAGER_SHOT=1 (diagnostic): keep eager engaged with a
 * --screenshot-on-frame target; the dump then drains the present thread,
 * renders from the scene the producer holds, and hands the display back.
 * -1 until read. */
static int s_eager_shot = -1;


/** @brief Read OPENPETE_PACE_LATE_BUDGET_US once: how much tick lateness an intermediate present may spend.
 *
 * The host loop admits an intermediate present that overruns the next sim
 * deadline by up to this budget when the tail cost bound (EMA plus three
 * deviations) fits in it. 0, the default, keeps the strict rule: never start
 * a present that could overrun the tick's start. The cap of 2000 us keeps
 * the budget under the slip threshold, so it costs phase, never the tick
 * grid.
 * @return The budget in nanoseconds, 0 to 2 000 000. */
static op_time_t pace_late_budget_ns(void)
{
    static op_time_t v = -1;
    if (v < 0) {
        const char *e = getenv("OPENPETE_PACE_LATE_BUDGET_US");
        long us = (e && *e) ? atol(e) : 0;
        if (us < 0) us = 0;
        if (us > 2000) us = 2000;
        v = (op_time_t)us * 1000;
    }
    return v;
}

/** @brief Read OPENPETE_PACE_SHED_K once: the multiplier on the cost deviation in the present-shed margin.
 *
 * The host loop sheds an intermediate present whose predicted cost does
 * not fit before the sim deadline; the predicted cost is the EMA plus k
 * deviations, or EMA plus EMA/4 at k = 0, the default. The two are A/B'd on
 * one machine through this variable.
 * @return k, 0 or greater. */
static int pace_shed_margin_k(void)
{
    static int k = -1;
    if (k < 0) {
        const char* e = getenv("OPENPETE_PACE_SHED_K");
        /* Default 0 = the centre-only bound (EMA + EMA/4). k=3 predicts better
         * (miss rate 19 % -> 5 % over 49k recorded presents) without a
         * measurable end-to-end difference, and k=4 collapses interpolation
         * (about 16 intermediate presents in a run instead of ~16000) because
         * a bound that wide almost never fits the budget. */
        k = (e && *e) ? atoi(e) : 0;
        if (k < 0) k = 0;
    }
    return k;
}

/** @brief Record a present of the given kind in the pace log with the last alpha and its pairing flag.
 *
 * @param kind The pace kind, OP_PACE_CANONICAL or OP_PACE_INTERMEDIATE. */
static void pace_record_present(uint8_t kind)
{
    openpete_pace_record_present(kind, openpete_pace_last_alpha(),
                                 openpete_pace_last_alpha_paired());
}

/** @brief Commit the canonical present's 2D capture, then resolve the framebuffer windows it names.
 *
 * Runs once per canonical present, after the fade commit and before any
 * re-extract or present of the tick. The 2D-UI overlay (fade, letterbox,
 * menu panel, 2D sprites and lines) is captured from the canonical render's
 * scene before a re-extract wipes it; sub-ticks only re-apply it, since their
 * scene already holds the re-applied set. With the spine off (unlock, paused,
 * interpolation disabled, headless) the capture still commits, so the carry
 * always holds the last canonical present's 2D set for the headless
 * intermediate leg and for the first sub-tick present after unlock. A pending
 * framebuffer-window copy is then composed from that scene, which by now
 * holds the tick's 2D, and cropped into its window before the tick's
 * re-extract and present, so the present that first draws the window draws
 * its image. The device belongs to the present thread while eager production
 * is engaged, so the compose takes it back and returns it. Both steps are a
 * no-op without a native scene. */
static void run_canonical_capture(void)
{
    if (!openpete_native_scene_enabled()) return;
    openpete_native_scene_capture_overlay();
    if (!openpete_fb_snapshot_pending()) return;
    const int pt_was = openpete_present_thread_borrow();
    openpete_native_render_snapshot_fb_window();
    openpete_present_thread_return(pt_was);
}

/** @brief Hold back the canonical present owed during the skip-to-level present fast-forward, and owe the latest one again when it ends.
 *
 * While the fast-forward is up the canonical present block does not run.
 * Each owed present (openpete_present_pending) is consumed here instead,
 * and the native scene's 2D accumulation is bounded to it
 * (openpete_native_scene_hold_2d) as its capture would have bounded it;
 * without that the accumulation spans the whole hold and the first present
 * after it draws the hold's 2D (the load screen's text and pictures) over
 * the level. On the first iteration after the fast-forward ends, a present
 * held back during it is owed again, so the latest held scene is presented
 * at once, as it was while the flag stayed raised through the hold. Runs
 * before the canonical present block on every iteration. */
static void held_present_gate(void)
{
    static int s_owed;
    if (openpete_skip_present_until_level) {
        if (!openpete_present_pending) return;
        openpete_present_pending = 0;
        s_owed = 1;
        if (openpete_native_scene_enabled()) openpete_native_scene_hold_2d();
        return;
    }
    if (s_owed) openpete_present_pending = 1;
    s_owed = 0;
}

/** @brief Fold the budget left before the present deadline into the decaying budget peak the probe waits for.
 *
 * Decay first, then take the max: without the decay this is a high-water
 * mark that one long budget pins forever, and the probe would then wait for
 * a moment that no longer comes.
 * @param sp The predictor.
 * @param deadline The deadline the present must beat. */
static void shed_note_budget(shed_predictor_t* sp, op_time_t deadline)
{
    const op_time_t here = deadline - op_now();
    sp->budget_peak -= sp->budget_peak / 64;
    if (here > sp->budget_peak) sp->budget_peak = here;
}

/** @brief Judge one intermediate present against the deadline: strict admit, late admit, probe or shed.
 *
 * The strict margin is the cost EMA plus OPENPETE_PACE_SHED_K deviations
 * (EMA plus a quarter at k = 0); the late admit spends up to
 * OPENPETE_PACE_LATE_BUDGET_US of tick lateness on the tail bound (EMA plus
 * three deviations). A verdict that admits resets the shed run; a probe
 * admits one present after PACE_SHED_PROBE_EVERY consecutive sheds once the
 * budget left reaches three quarters of the decaying peak (at most half a
 * tick).
 * @param sp The predictor.
 * @param deadline The deadline the present must beat.
 * @return The verdict. */
static shed_verdict_t shed_admit(shed_predictor_t* sp, op_time_t deadline)
{
    /* Cover the tail, not the centre. k=3 takes the miss rate from
     * ~19 % to ~5 % on the recorded cost series; the cost of the wider
     * bound is shedding a little earlier, which is the trade this
     * predicate exists to make. */
    const int kk = pace_shed_margin_k();
    const op_time_t margin = kk
        ? sp->cost_ema + (op_time_t)kk * sp->cost_dev
        : sp->cost_ema + sp->cost_ema / 4;
    /* Lateness budget: how far a present may push the tick's start.
     *
     * The frame refused right before a tick is NOT a duplicate of the
     * canonical that follows - measured, it sits 0.083 of a tick
     * behind it against a 0.069 normal step - so the strict rule
     * discards real motion at the worst moment, just before the
     * display holds one image for the length of the tick.
     *
     * Spending the budget is gated on the TAIL bound rather than the
     * centre one, because the cost distribution is right-skewed (p50
     * 436 us, p90 707 us): admitting on a centre estimate is how a
     * 100 us trade becomes a multi-millisecond late tick. Zero (the
     * default) keeps the strict rule. */
    const op_time_t late_budget = pace_late_budget_ns();
    const op_time_t tail_margin = sp->cost_ema + 3 * sp->cost_dev;
    const int fits_strict = op_now() + margin <= deadline;
    const int fits_late   = late_budget > 0 &&
        op_now() + tail_margin <= deadline + late_budget;
    if (fits_strict || fits_late) {
        sp->shed_run = 0;
        return fits_strict ? SHED_ADMIT_STRICT : SHED_ADMIT_LATE;
    }
    /* A shed produces no measurement, so a predictor that only
     * learns from presents that RAN stops learning the moment it
     * starts shedding: the estimate latches at whatever the last
     * admitted present cost - a cold start, a shader compile, one
     * slow scene - and sheds forever after, with nothing able to
     * contradict it. Observed doing exactly that: 16 presents
     * admitted, then every subsequent one shed for the rest of
     * the run.
     *
     * So let one through periodically. The probe costs one late
     * tick at worst and is the only way the estimate can come
     * back down. */
    /* Probe only where a probe can do least harm: with at least
     * half the tick period still available. A counter-based probe
     * admits an expensive present at an arbitrary moment, which
     * on slow hardware means one deliberately late tick every N -
     * measured as a 11.7 ms tick p99 on a software rasterizer,
     * caused entirely by the mechanism meant to protect it.
     * Choosing the moment with the most budget bounds the damage
     * to the case where a present genuinely cannot fit in half a
     * tick, where a rare late tick is the price of ever
     * discovering the cost came back down. */
    const op_time_t budget = deadline - op_now();
    /* Relative to what this machine actually offers, not to the
     * tick period: an absolute threshold is a threshold some
     * machines never reach, and there the probe silently stops
     * existing. Three quarters of the observed peak keeps the
     * "least harm" intent - the probe still waits for a
     * near-best moment - while guaranteeing such a moment recurs.
     * Capped at half a tick so a machine with plenty of headroom
     * behaves exactly as before. */
    op_time_t probe_at = sp->budget_peak - sp->budget_peak / 4;
    if (probe_at > (op_time_t)g_openpete_tick_period_ns / 2)
        probe_at = (op_time_t)g_openpete_tick_period_ns / 2;
    if (++sp->shed_run >= PACE_SHED_PROBE_EVERY && budget >= probe_at) {
        sp->shed_run = 0;
        return SHED_PROBE;
    }
    return SHED_SHED;
}

/** @brief Feed one measured intermediate present's cost into the predictor's mean and deviation.
 *
 * Deviation first: it is measured against the PREVIOUS centre, so updating
 * the centre first would shrink every deviation toward zero and collapse
 * the bound back onto the mean. The count stops at PACE_COST_WARMUP.
 * @param sp The predictor.
 * @param span The present's measured span in nanoseconds. */
static void shed_learn(shed_predictor_t* sp, op_time_t span)
{
    if (sp->n) {
        const op_time_t d = span > sp->cost_ema
            ? span - sp->cost_ema : sp->cost_ema - span;
        sp->cost_dev = (sp->cost_dev * 7 + d) / 8;
    }
    sp->cost_ema = sp->n
        ? (sp->cost_ema * 7 + span) / 8
        : span;
    if (sp->n < PACE_COST_WARMUP) sp->n++;
}

/* Documented at the declaration in host_loop_internal.h. */
void present_record_shown(openpete_frame_time_log_t* ftl, int kind)
{
    if (op_prof_on())
        op_prof_present((uint32_t)openpete_canon_tick, kind);
    openpete_frame_time_log_present(ftl, openpete_frame_published());
}

/* Documented at the declaration in host_loop_internal.h. */
bool host_world_live(const host_session_t* S, const host_iter_t* it, op_time_t now)
{
    return S->last_canon_present_ns != 0 &&
        (now - S->last_canon_present_ns) <= 6 * it->sim_period_ns;
}

/** @brief Report whether sub-tick presentation is live: a window, a native scene, paced, interpolation on and enabled, and not paused without a step.
 *
 * The canonical lag re-extract and eager engagement need it with a
 * registered re-extract; eager disengages when it fails; the inline
 * intermediate present needs it with no skip fast-forward and no present
 * made this iteration.
 * @param S The session: the options and the pause flags.
 * @return true when sub-tick presents may run. */
static bool host_subtick_live(const host_session_t* S)
{
    return !S->opts->headless && openpete_native_scene_enabled() &&
        !openpete_unlock_fps && g_openpete_interp_enabled &&
        openpete_interp_enabled() && !(S->paused && !S->step_one);
}

/* Documented at the declaration in host_loop_internal.h. */
void host_present_request_screenshot(void)
{
    s_screenshot_request = 1;
}

/** @brief Engage or disengage eager production at a canonical present on the full engage predicate.
 *
 * Eager production takes the whole present path from here: the canonical
 * scene is the first of the span's plan, and the present thread shows it.
 * Never with a recording or a screenshot target -- those read the inline
 * present -- and never under the virtual clock (--record-cfr): the sim
 * pacer is then the clock's one writer, and the present thread's wall-clock
 * waits have no meaning against it. OPENPETE_EAGER_SHOT keeps it engaged
 * with a --screenshot-on-frame target. The OPENPETE_EAGER_HOLD_S term is
 * read last, so its clock starts at the first present every other term
 * admits.
 * @param opts The parsed options.
 * @param spine_on Whether the canonical present runs on the sub-tick spine. */
static void eager_engage_update(const openpete_options_t* opts, bool spine_on)
{
    if (s_eager_shot < 0) { const char *e = getenv("OPENPETE_EAGER_SHOT"); s_eager_shot = (e && *e && *e != '0'); }
    const int eager_now = spine_on && openpete_present_thread_enabled() &&
        !opts->record_path && !op_virtual_clock_active() && (opts->screenshot_frame_count == 0 || s_eager_shot) &&
        opts->screenshot_intermediate_count == 0 &&
        opts->screenshot_presents_start < 0 && eager_hold_done();
    eager_set_active(eager_now);
}

/** @brief Disengage eager production when sub-tick presentation is no longer live.
 *
 * Eager disengages the moment its gate closes on any iteration (a pause
 * pressed mid-span), so the inline path never presents while the present
 * thread still holds the display. Re-engaging waits for the next canonical
 * tick, which owns the plan. After a canonical present in the same
 * iteration it changes nothing: its terms are a subset of the negated
 * engage predicate.
 * @param S The session. */
static void eager_disengage_check(const host_session_t* S)
{
    if (eager_active() && !host_subtick_live(S)) eager_set_active(0);
}

/** @brief Plan and produce the eager span of this canonical present from the pacer's render grid and sim deadlines.
 * @param S The session: the pacer and the world-liveness stamp.
 * @param it The iteration: the VBL period. */
static void eager_canonical_present(const host_session_t* S, const host_iter_t* it) OP_REQUIRES(sim)
{
    host_trace_present(HT_P_CANON_EAGER);
    const host_pacer_t* P = &S->pacer;
    const op_time_t period = host_pacer_render_period_ns(P);
    const op_time_t now = op_now();
    eager_canonical(now, host_pacer_render_slot(P), period,
                    host_pacer_sim_deadline(P, 2), host_pacer_sim_deadline(P, 1),
                    host_world_live(S, it, now));
}

/** @brief Close the bug-report recorder's open present for an inline re-extract, with the alpha the re-extract resolved and no ring slot. */
static void subtick_end_inline(void) OP_REQUIRES(sim)
{
    int paired = 0;
    const double alpha = openpete_pace_extract_alpha(&paired);
    openpete_bugrec_subtick_end(alpha, paired, -1, 0u, 0u, 0);
}

/** @brief Re-extract the canonical present at the lagged alpha the intermediates use, on the inline strategy.
 *
 * Sub-tick spine: the game's render just built the scene at the latest pose
 * (tick N), but the intermediates that follow present the 1-tick-lagged
 * interpolation (prev->curr). Showing the un-lagged canonical frame between
 * lagged intermediates makes every object jump forward one tick at the
 * canonical present and snap back on the next intermediate, a 30 Hz strobe
 * whenever the camera moves; re-extracting here puts every present on one
 * continuous, 1-tick-lagged timeline. Raises g_openpete_canon_lag_leg for
 * the re-extract.
 * @param lag_re The registered re-extract.
 * @return The re-extract's duration in nanoseconds. */
static op_time_t inline_canonical_extract(openpete_native_reextract_fn lag_re) OP_REQUIRES(sim)
{
    host_trace_present(HT_P_CANON_LAG);
    OP_PROF_SCOPE("scene");      /* the scene's production: the profiler's root region */
    uint64_t pt0 = openpete_phase_timers_on() ? openpete_phase_now_us() : 0;
    g_openpete_canon_lag_leg = 1;
    vp_set_present_target();     /* even display-slot time for this present */
    const op_time_t rex_t0 = op_now();
    openpete_bugrec_subtick_begin(OP_PACE_CANONICAL, BUGREC_SUB_INLINE);
    { OP_PROF_SCOPE("scene.rex");
      lag_re(&g_game.cpu); }
    subtick_end_inline();
    { OP_PROF_SCOPE("scene.sig_report");
      openpete_native_scene_sig_report(1); }
    const op_time_t extract_ns = op_now() - rex_t0;
    g_openpete_canon_lag_leg = 0;
    if (pt0) openpete_phase_add(OPPT_REX_TOTAL, openpete_phase_now_us() - pt0);
    return extract_ns;
}

/** @brief Choose the canonical present's strategy: engage or disengage eager, then plan the eager span or run the inline lag re-extract.
 *
 * The spine is on with sub-tick presentation live and a re-extract
 * registered. Headless has no live present stream, so the canonical dump
 * stays the pure canonical frame: the presentation-golden intermediate
 * corpus enables interp capture headless (for the ring), which would
 * otherwise lag the canonical -vk; the explicit alpha 0.5 capture owns the
 * sub-tick leg instead.
 * @param S The session.
 * @param it The iteration.
 * @param lag_re The registered re-extract, or NULL.
 * @return The inline lag re-extract's duration, 0 under eager or with the spine off. */
static op_time_t present_canonical_strategy(const host_session_t* S, const host_iter_t* it,
                                            openpete_native_reextract_fn lag_re) OP_REQUIRES(sim)
{
    const bool spine_on = lag_re && host_subtick_live(S);
    host_trace_bit(HT_F_SPINE, spine_on);
    eager_engage_update(S->opts, spine_on);
    if (eager_active()) {
        eager_canonical_present(S, it);
        return 0;
    }
    return spine_on ? inline_canonical_extract(lag_re) : 0;
}

/** @brief Render the canonical present inline: the mods' tick commit and present hooks, then the native frame.
 *
 * Under OPENPETE_GPU_AUDIT_SELFTEST the first such present runs with
 * g_openpete_guest_running raised, so the GPU audit records its Vulkan
 * calls. The PTS tags the present for --record, whose readback ring
 * captures the on-screen frame without a GPU stall. */
static void inline_canonical_render(void) OP_REQUIRES(sim)
{
    openpete_mods_present_flush(&g_game.cpu, 1);
    const int audit_arm = gpu_audit_selftest_arm();
    if (audit_arm) g_openpete_guest_running = 1;
    { OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_RENDER); OP_PROF_SCOPE("render");
      openpete_native_render_frame((uint64_t)(op_now() / 1000)); }
    if (audit_arm) g_openpete_guest_running = 0;
}

/** @brief Take the player screenshot a binding or the pointer requested, a readback of the frame the window just showed.
 *
 * One unsuffixed PNG of the offscreen the present blitted, so it carries the
 * sub-tick scene, mods, postfx and overlays as presented, rather than the
 * reference-aligned recompose the --screenshot-on-frame variants need.
 * Native display only. While the present thread holds the display, the
 * request goes to it (openpete_png_request): it copies its next presented
 * image in that present's own submission and
 * the PNG writer converts, encodes and writes it, so neither this thread
 * nor the display waits. Otherwise, and under OPENPETE_SHOT_SYNC, this
 * thread reads the offscreen back itself and the writer encodes. The bug
 * report's image stays synchronous: the bundle reads the file right
 * after. */
static void present_player_screenshot(void) OP_REQUIRES(sim)
{
    char bug_path[1400];
    if (openpete_bugreport_take_shot(bug_path, sizeof bug_path)) {
        /* The bundle's image and the display reports: the device and the
         * present thread's counters are this thread's while borrowed. */
        const int pt_was = openpete_present_thread_borrow();
        const int ok = openpete_native_render_enabled() &&
                       openpete_native_render_screenshot(bug_path, "bug report") == 0;
        openpete_bugreport_display_service(ok);
        openpete_present_thread_return(pt_was);
    }
    if (!s_screenshot_request && !openpete_host_mouse_take_screenshot_request()) return;
    s_screenshot_request = 0;
    char path[1024];
    if (!openpete_native_render_enabled()) {
        OPL_WARN_F("screenshot",
                   "no native renderer: nothing to capture");
    } else if (openpete_screenshot_user_path(path, sizeof path) != 0) {
        return;
    } else if (openpete_present_thread_active() && !openpete_png_async_inline()) {
        /* The present thread copies its next presented image in that
         * present's submission (the native render's shot cycle); the
         * display stays where it is. */
        (void)openpete_png_request(path, "player");
    } else {
        const int pt_was = openpete_present_thread_borrow();
        openpete_native_render_screenshot_async(path, "player");
        openpete_present_thread_return(pt_was);
    }
}

/** @brief Run the canonical present's device services and captures, shared by both strategies.
 *
 * The terrain-cache and occlusion services on the headless path (each a
 * no-op with a window), the native target captures, the player screenshot, the every-tick identity audit (between the two
 * dump blocks, so both see the untouched canonical scene; it restores the
 * scene before returning) and the presentation-golden intermediate leg.
 * @param opts The parsed options.
 * @param shot_key The present's capture key.
 * @param capture_canon_present Whether the present-sequence capture owes this present. */
static void present_canonical_services(const openpete_options_t* opts, int shot_key,
                                       bool capture_canon_present) OP_REQUIRES(sim)
{
    openpete_native_tercache_service_headless();
    openpete_native_occl_service_headless();
    openpete_capture_canon_native(opts, shot_key, capture_canon_present);
    present_player_screenshot();
    if (openpete_ident_audit_armed()) {
        const int pt_was = openpete_present_thread_borrow();
        openpete_ident_audit_present(&g_game.cpu, openpete_frame,
                                     openpete_canon_tick,
                                     opts->screenshot_output);
        openpete_present_thread_return(pt_was);
    }
    openpete_capture_canon_intermediate(opts, &g_game.cpu);
}

/** @brief Close out an inline canonical present: profile and frame-time-log records, then the pace record and span.
 *
 * The frame-time log keeps one record per actual present; the pace span
 * runs from the present's start through this call.
 * @param out The run's outputs.
 * @param t0 The present's start.
 * @param extract_ns The lag re-extract's duration, 0 without one. */
static void inline_canonical_tail(host_outputs_t* out, op_time_t t0, op_time_t extract_ns) OP_REQUIRES(sim)
{
    present_record_shown(&out->ftl, OP_PROF_CANONICAL);
    pace_record_present(OP_PACE_CANONICAL);
    openpete_pace_present_span(OP_PACE_CANONICAL, t0, op_now(), extract_ns);
}

/** @brief Present the tick the game drew: canon tick, EndScene, the liveness stamp, the commits, the strategy, the render, the services and the tail.
 *
 * The canonical draw-tick counter's Nth increment is the same sim moment in
 * every run, unlike openpete_frame, and keys --screenshot-on-tick. Without a
 * native scene --record captures here, before EndScene stamps the FPS
 * overlay. The world-liveness stamp is taken at EndScene; the present is
 * bracketed from the canonical inject on, so the pace log names the one
 * cost shedding can never avoid. The per-canonical commit latches the
 * render paths this frame drew and the fade commit rolls the fade and
 * freeze one-tick flags, on every canonical present whatever the strategy.
 * @param S The session.
 * @param it The iteration; sets did_present. */
static void present_canonical(host_session_t* S, host_iter_t* it) OP_REQUIRES(sim)
{
    const openpete_options_t* opts = S->opts;
    openpete_canon_tick++;
    bk2_feed_map_present();
    const int shot_key = opts->screenshot_ticks_keyed ? (int)openpete_canon_tick
                                                      : (int)openpete_feed_key_now();
    const bool capture_canon_present = openpete_capture_canon_reference(opts, shot_key);
    if (!openpete_native_scene_enabled())
        openpete_record_capture_frame((uint64_t)(op_now() / 1000));
    PsyX_EndScene();
    openpete_present_pending = 0;
    it->did_present = true;
    host_trace_present(HT_P_CANON_INLINE);
    S->last_canon_present_ns = op_now();
    const op_time_t t0 = op_now();
    openpete_pace_inject_canonical();  /* fault injection, off by default */
    openpete_native_run_commit();
    openpete_native_reextract_fn lag_re = openpete_native_get_reextract();
    openpete_native_scene_commit_fade();
    run_canonical_capture();
    const op_time_t extract_ns = present_canonical_strategy(S, it, lag_re);
    openpete_capture_present_begin(opts);
    if (!eager_active()) inline_canonical_render();
    present_canonical_services(opts, shot_key, capture_canon_present);
    if (!eager_active()) inline_canonical_tail(&S->out, t0, extract_ns);
}

/** @brief Present one inline intermediate frame: the re-extract at interpolated gamestate while the world is live, the render, the records, and the cost the predictor learns.
 *
 * While canonical presents are not flowing (CD load, level transition, a
 * long producer stall) re-extracting reads RAM mid-overwrite, so the last
 * composed scene is presented unchanged. The PTS tags the present for
 * --record, so a --render-fps N recording is about N fps VFR. The
 * present-sequence capture takes the sub-tick frames after it.
 * @param S The session.
 * @param it The iteration.
 * @param native_re The registered re-extract. */
static void present_inline_intermediate(host_session_t* S, const host_iter_t* it,
                                        openpete_native_reextract_fn native_re) OP_REQUIRES(sim)
{
    host_trace_intermediate();
    const op_time_t pace_span_t0 = op_now();
    op_time_t pace_extract_ns = 0;
    openpete_pace_inject_present();   /* fault injection, off by default */
    if (host_world_live(S, it, op_now())) {
        host_trace_bit(HT_F_INTER_REX, 1);
        /* The in-between scene's production: the profiler's root region. */
        OP_PROF_FLAGS_SCOPE("scene", OP_PROF_F_INTERMEDIATE);
        uint64_t pt0 = openpete_phase_timers_on() ? openpete_phase_now_us() : 0;
        vp_set_present_target();      /* even display-slot time for this present */
        const op_time_t ex0 = op_now();
        openpete_bugrec_subtick_begin(OP_PACE_INTERMEDIATE, BUGREC_SUB_INLINE);
        { OP_PROF_SCOPE("scene.rex");
          native_re(&g_game.cpu); }   /* rebuild s_scene @ interpolated gamestate */
        subtick_end_inline();
        s_inline_rex++;
        { OP_PROF_SCOPE("scene.sig_report");
          openpete_native_scene_sig_report(0); }
        pace_extract_ns = op_now() - ex0;
        if (pt0) openpete_phase_add(OPPT_REX_TOTAL, openpete_phase_now_us() - pt0);
    } else {
        openpete_bugrec_subtick_begin(OP_PACE_INTERMEDIATE, BUGREC_SUB_REPEAT);
        subtick_end_inline();
    }
    /* Mod draw service: present hooks at sub-tick alpha (no commit). */
    openpete_mods_present_flush(&g_game.cpu, 0);
    { OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_RENDER); OP_PROF_FLAGS_SCOPE("render", OP_PROF_F_INTERMEDIATE);
      openpete_native_render_frame((uint64_t)(op_now() / 1000)); }
    present_record_shown(&S->out.ftl, OP_PROF_INTERMEDIATE);
    pace_record_present(OP_PACE_INTERMEDIATE);
    /* The span feeds the predictor and the pace log, so attribution can
     * name present CPU as a cause of a late tick. */
    const op_time_t span = op_now() - pace_span_t0;
    openpete_pace_present_span(OP_PACE_INTERMEDIATE, pace_span_t0,
                               pace_span_t0 + span, pace_extract_ns);
    shed_learn(&s_shed, span);
    openpete_capture_subtick_present(S->opts);
}

/** @brief Run the inline strategy between canonical presents: the budget peak, the shed predictor once warm, then the intermediate present unless shed.
 *
 * The predictor gives no verdict before PACE_COST_WARMUP measured presents
 * or under unlock; a shed writes a PACE_SHED_NO_BUDGET record with the cost
 * estimate and the margin left.
 * @param S The session.
 * @param it The iteration.
 * @param deadline The deadline the present must beat.
 * @param native_re The registered re-extract. */
static void present_inline_between(host_session_t* S, const host_iter_t* it, op_time_t deadline,
                                   openpete_native_reextract_fn native_re) OP_REQUIRES(sim)
{
    shed_note_budget(&s_shed, deadline);
    if (s_shed.n >= PACE_COST_WARMUP && !openpete_unlock_fps) {
        host_trace_bit(HT_F_PREDICTOR, 1);
        switch (shed_admit(&s_shed, deadline)) {
        case SHED_ADMIT_STRICT: host_trace_admit(1); break;
        case SHED_ADMIT_LATE:   host_trace_admit(0); break;
        case SHED_PROBE:        host_trace_verdict(HT_V_PROBE); break;
        case SHED_SHED:
            host_trace_verdict(HT_V_SHED);
            host_trace_present(HT_P_SHED);
            openpete_pace_shed(OP_PACE_INTERMEDIATE, PACE_SHED_NO_BUDGET,
                               (int64_t)s_shed.cost_ema,
                               (int64_t)(deadline - op_now()));
            return;
        }
    }
    present_inline_intermediate(S, it, native_re);
}

/* ---- headless forced re-extract -------------------------------------------
 * Headless never starts eager production or the inline intermediate present,
 * so the oracle never ran the re-extract body (the guest render code the
 * sub-tick view brackets) against gameplay RAM. With
 * OPENPETE_HEADLESS_REEXTRACT=1 every non-drawing VBL of a headless run
 * re-extracts the scene at alpha 0.5 of the newest tick through the
 * registered re-extract (the view's open, body and restore that eager and
 * the inline present run) and then makes the mods' sub-tick present flush,
 * with no present thread and no render. The world-liveness bound is the
 * windowed one, 6 VBLs since the last canonical present, counted in VBLs so
 * the run is the same on every host. */
static OP_ONCE_INT s_hrx_on = -1;
/* VBLs since the last canonical present; starts past the bound, so nothing
 * runs before the first drawn tick. */
static _Atomic uint32_t s_hrx_since_canon = 1000u;
/* Re-extracts run, and those the interpolation ring bracketed. */
static _Atomic uint32_t s_hrx_rex;
static _Atomic uint32_t s_hrx_paired;
#define HRX_LIVE_VBLS 6u

bool host_headless_reextract_on(void)
{
    int on = op_once_int_get(&s_hrx_on);
    if (on < 0) {
        const char* e = getenv("OPENPETE_HEADLESS_REEXTRACT");
        on = op_once_int_fill(&s_hrx_on, (e && *e && *e != '0') ? 1 : 0);
    }
    return on > 0;
}

/** @brief Re-extract the scene at alpha 0.5 on a headless non-drawing VBL, then flush the mods' sub-tick present.
 *
 * Runs on a headless iteration that ran the game fiber and presented
 * nothing, while the world is live (a canonical present within
 * HRX_LIVE_VBLS VBLs) and outside a skip fast-forward. The present target is
 * the ring's newest stamp plus half a tick, the instant the deterministic
 * ring maps to alpha 0.5.
 * @param S The session: the options.
 * @param it The iteration.
 * @param re The registered re-extract, or NULL. */
static void headless_forced_reextract(const host_session_t* S, const host_iter_t* it,
                                      openpete_native_reextract_fn re) OP_REQUIRES(sim)
{
    if (it->did_present) { s_hrx_since_canon = 0; return; }
    if (!it->yield_now) return;
    if (s_hrx_since_canon < 1000u) s_hrx_since_canon++;
    if (!S->opts->headless || !re || openpete_skip_present_until_level ||
        s_hrx_since_canon > HRX_LIVE_VBLS)
        return;
    openpete_interp_set_present_target(openpete_interp_curr_tick_ns() +
                                       g_openpete_tick_period_ns / 2);
    { OP_PROF_FLAGS_SCOPE("scene", OP_PROF_F_INTERMEDIATE);
      re(&g_game.cpu); }
    openpete_interp_set_present_target(0);
    s_hrx_rex++;
    if (openpete_pace_last_alpha_paired()) s_hrx_paired++;
    openpete_mods_present_flush(&g_game.cpu, 0);
}

/** @brief Log the headless forced re-extract's exit row when OPENPETE_HEADLESS_REEXTRACT is set.
 *
 * `headless-rex: re-extracts=R paired=P view_opens=O view_restores=V
 * spans=S`: a gate reads R, O, V and S (the spans producers staged into the
 * view) as its engagement counts. */
static void headless_reextract_report(void)
{
    if (!host_headless_reextract_on()) return;
    OPL_INFO_F("headless-rex", "re-extracts=%u paired=%u view_opens=%u view_restores=%u spans=%u",
               atomic_load(&s_hrx_rex), atomic_load(&s_hrx_paired),
               openpete_subtick_view_opens(), openpete_subtick_view_restores(),
               openpete_subtick_view_spans_total());
}

/** @brief Fill the display between canonical presents: the eager disengage check, then eager production or the inline intermediate present, then the paused repaint.
 *
 * The present deadline is the next sim deadline the pacer will step to (on
 * a sim_due iteration the tail has not advanced it yet). Under eager, a
 * non-drawing iteration produces whatever of the plan still fits 0.5 ms
 * before it; nothing is presented from here. The inline intermediate
 * present runs only at a render rate above the sim rate, on an iteration
 * that made no canonical present, outside a skip fast-forward.
 * @param S The session.
 * @param it The iteration. */
static void present_between(host_session_t* S, const host_iter_t* it) OP_REQUIRES(sim)
{
    openpete_native_reextract_fn native_re = openpete_native_get_reextract();
    eager_disengage_check(S);
    /* openpete_unlock_fps is computed at the loop tail, so on the first
     * iteration it does not yet reflect a skip fast-forward armed at
     * boot; read the fast-forward flag directly as well. */
    const bool native_intermediate = host_subtick_live(S) &&
        !openpete_skip_present_until_level && !it->did_present && native_re != NULL;
    host_trace_bit(HT_F_NATIVE_INTER, native_intermediate);
    const op_time_t deadline = host_pacer_present_deadline(&S->pacer, it->sim_due);
    if (eager_active()) {
        if (eager_idle(host_world_live(S, it, op_now()), !it->did_present && native_re,
                       deadline - 500000, host_pacer_render_period_ns(&S->pacer)))
            host_trace_present(HT_P_EAGER_PRODUCE);
    } else if (native_intermediate) {
        present_inline_between(S, it, deadline, native_re);
    } else if (host_headless_reextract_on()) {
        headless_forced_reextract(S, it, native_re);
    }
    /* Paused repaint: no tick runs, so no other present fires. */
    if (S->paused && !S->step_one && !it->did_present) {
        host_paused_repaint(S->opts);
        host_trace_present(HT_P_PAUSED_REPAINT);
    }
}

/* Documented at the declaration in host_loop_internal.h. */
void host_present(host_session_t* S, host_iter_t* it) OP_REQUIRES(sim)
{
    held_present_gate();
    if (openpete_present_pending && !openpete_skip_present_until_level)
        present_canonical(S, it);
    present_between(S, it);
}

/* Documented at the declaration in host_loop_internal.h. */
void host_present_exit_report(void)
{
    eager_exit_report(s_inline_rex);
    headless_reextract_report();
}

#endif /* OPENPETE_HAS_PSYCROSS */
