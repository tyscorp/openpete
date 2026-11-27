// core/runtime/eager.c - eager production: at each canonical tick, plan
// which display instants of the coming span get a scene, produce them on the
// sim thread while it is idle, and publish them into the scene ring for the
// present thread to show on their instants.

#include "eager.h"
#include "host_loop_internal.h"
#include "run_internal.h"

#include "openpete_bugrec_subtick.h"
#include "openpete_discontinuity.h"
#include "openpete_eager_ladder.h"
#include "openpete_fiber.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_log.h"
#include "openpete_mods.h"
#include "openpete_native_render.h"
#include "openpete_pace_log.h"
#include "openpete_present_thread.h"
#include "openpete_profile.h"
#include "openpete_time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

#include <stdatomic.h>

/* The cadence policy (--cadence), read by every span plan. */
static _Atomic(int) s_cadence = OPENPETE_CADENCE_EVEN;

/* Documented at the declaration in openpete.h. */
void openpete_set_cadence(int cadence)
{
    const int v = cadence == OPENPETE_CADENCE_FILL ? OPENPETE_CADENCE_FILL : OPENPETE_CADENCE_EVEN;
    if (atomic_exchange(&s_cadence, v) != v)
        OPL_INFO_F("eager", "cadence -> %s", v == OPENPETE_CADENCE_FILL
                   ? "fill (every scene the span affords, on the display grid)"
                   : "even (every level-th display slot)");
}

/* Documented at the declaration in openpete.h. */
int openpete_get_cadence(void) { return atomic_load(&s_cadence); }

#ifdef OPENPETE_HAS_PSYCROSS

/* Eager production.
 *
 * The pull model presented one scene at a time, extracting it on the sim
 * thread at the moment the pacer decided it was due -- and refused the
 * slots that fell inside the tick's protective margin, because extracting
 * and executing the guest share this thread. Nothing forces extraction to
 * happen at present time. Every present between tick N and tick N+1
 * interpolates the span [N-1, N], whose both endpoints are already in the
 * interp ring the moment tick N lands; so at that moment the whole span is
 * extractable, and this thread is about to be idle for most of a tick.
 *
 * So: at the canonical tick, PLAN which grid instants of the coming span
 * get a scene; then PRODUCE them one at a time whenever this thread is
 * idle, publishing each into the scene ring for the present thread, which
 * shows them on their instants -- through the next tick's execution. The
 * deadline check sits BETWEEN scenes, never inside one, so the guest never
 * resumes on interpolated RAM: production is scene-atomic. A scene whose
 * instant has already passed is skipped (except the canonical, which also
 * carries the tick boundary for mods).
 *
 * Running out of CPU inverts the failure mode. The pull model found out at
 * the boundary, one present at a time, and lost a contiguous run at the END
 * of the span. Here the budget (span length minus what this thread must
 * still do in it) and the per-scene cost are known before anything is
 * produced, so the shape of the degradation is chosen up front: a divisor
 * ladder (every 2nd slot, every 3rd ...) keeps the cadence uniform on a
 * fixed refresh. The trigger is a division, not a threshold on a curve.
 * Hysteresis is asymmetric -- a span that cannot afford the standing level
 * plans its own span lower at once, the standing level drops when such
 * spans recur, and it climbs only after several consecutive spans that
 * afford a higher rate -- because a rate that oscillates is worse than a
 * steady lower one, and one costly span is not a lower capacity.
 *
 * The lag. The spine shows span [N-1, N] from the moment tick N lands, at
 * alpha = (t - stamp_N) / period, so alpha reaches 1 when tick N+1 is DUE --
 * and tick N+1 does not land until it has executed, ~6 ms later. The slots
 * inside that execution sit at alpha 1.0-1.2: they need pose N+1, which does
 * not exist yet. No amount of extraction ahead of time fills them; the
 * timeline itself is one tick-execution too short. So an eager scene for
 * display instant t animates the instant t - lag, where lag tracks the tail
 * of the tick's execution time: every slot through the execution then sits
 * at alpha < 1 of a span whose endpoints are both known, no extrapolation,
 * and the price is a CONSTANT few milliseconds of display latency in place
 * of a hold of the same length thirty times a second.
 *
 * The cadence policy (--cadence) chooses the shape on a display grid: even
 * runs the divisor ladder above; fill plans as many scenes as the span
 * affords, spread over the grid's slots (op_eager_spread_slot), its count
 * kept below afford by a deficit raised one scene per cut and cleared after
 * a clean run (op_eager_fill_step), the ladder's hysteresis in count space.
 *
 * OPENPETE_EAGER=0 disables (on by default). OPENPETE_EAGER_POLICY=greedy
 * swaps the divisor ladder for "as many scenes as fit, evenly spaced across
 * the span" -- the A/B partner for the question whether uniform-on-grid
 * beats more-frames-off-grid. OPENPETE_EAGER_LEVEL=n pins the divisor;
 * OPENPETE_EAGER_LAG_US=n pins the lag. */
#define EAGER_MAX_TARGETS 128
#define EAGER_COST_WARMUP 8
#define EAGER_HEADROOM_RUNS 8
/* A free span gives back one scene after this many consecutive spans that
 * finished with EAGER_COUNT_SPARE scenes of room; a ring cut's drain floor
 * holds for EAGER_DRAIN_RUNS spans. */
#define EAGER_COUNT_RUNS 16
#define EAGER_COUNT_SPARE 1
#define EAGER_DRAIN_RUNS 32
/* The presenter cap's backlog gate judges blocks of this many spans with
 * picks (about half a second of ticks) and arms after as many spans of
 * evidence, doubling after each disarm up to the maximum (about 8.6 s). */
#define EAGER_CAP_ARM_SPANS 16
#define EAGER_CAP_ARM_MAX 256
/* OPENPETE_EAGER_CAP_AB=<ms>: the presenter cap's in-binary A/B. Arm 0
 * runs with the cap off and arm 1 with it on, alternating every window;
 * OPENPETE_EAGER_CAP_AB_NULL=1 runs both arms with the configured cap (the
 * null A/B). The first quarter of each window settles and is not counted,
 * nor is an interval while the world is not live. */
typedef struct { op_time_t ns; unsigned presents, sup, spans, capped; } eager_cap_arm_t;
typedef struct {
    op_time_t window;                 /* arm length in nanoseconds; 0 = off */
    int       null_ab;                /* both arms run the configured cap */
    int       cap_cfg;                /* the cap setting OPENPETE_EAGER_CAP chose */
    int       arm;                    /* the arm of the open interval */
    int       counting;               /* the open interval is past the settle */
    op_time_t arm_start, t0;          /* the arm's start; the open interval's start */
    unsigned  pres0, sup0;            /* presents and ring retirements at t0 */
    eager_cap_arm_t acc[2];           /* counted time, presents, retired scenes, spans, capped spans per arm */
} eager_cap_ab_t;
static struct {
    int       active;                 /* the present thread owns the display */
    int       n, i;                   /* planned targets, next to produce */
    op_time_t target[EAGER_MAX_TARGETS];
    op_eager_ladder_t ladder;         /* the standing divisor and its run counts */
    int       level;                  /* divisor of the span being produced: every level-th slot */
    int       pinned;                 /* OPENPETE_EAGER_LEVEL */
    int       afford_pin;             /* OPENPETE_EAGER_AFFORD: force the budget's verdict (A/B) */
    int       cap_on;                 /* OPENPETE_EAGER_CAP (default 1): cap afford at the presenter's consumption while it is behind */
    op_eager_cap_t cap;               /* the presenter cap's backlog gate (op_eager_cap_step) */
    int       span_cap;               /* the open span's presenter cap on afford; 0 when none was computed */
    op_time_t span_cap_ns;            /* the open span's least free spacing under the cap; 0 when none */
    int       span_capped;            /* the presenter cap lowered the open span's scenes */
    unsigned  capped_spans;           /* spans the cap lowered */
    unsigned  picks_seen, starved_seen;   /* openpete_present_thread_picks() at the last plan */
    unsigned  sup_seen;               /* openpete_native_scene_ring_superseded() at the last plan */
    unsigned  span_sup;               /* scenes the ring retired unpresented between the last two plans */
    op_time_t cap_plan_at;            /* op_now() at the last plan; 0 before the first */
    eager_cap_ab_t ab;                /* OPENPETE_EAGER_CAP_AB */
    int       count_spare;            /* OPENPETE_EAGER_COUNT_SPARE: room a clean span leaves, in scenes */
    int       count_runs;             /* OPENPETE_EAGER_COUNT_RUNS: clean spans that give back one scene */
    int       rewarm_on;              /* OPENPETE_EAGER_REWARM (default 1): re-warm the cost averages at the world going live */
    int       live_prev;              /* world_live at the previous call that set it */
    int       rewarm_edge_on;         /* OPENPETE_EAGER_REWARM_EDGE (default 0): a level edge opens the re-warm window too */
    unsigned  edges_seen;             /* openpete_native_level_edges() at the previous call */
    unsigned  edge_windows;           /* re-warm windows a level edge opened */
    int       warm_left;              /* scenes left in the re-warm window */
    int       span_warm;              /* the open span was planned inside the re-warm window */
    int       policy;                 /* 0 divisor, 1 greedy */
    op_time_t cost_ema, cost_dev; int cost_n;   /* per-scene production cost */
    op_time_t vbl_ema;                /* cost of a non-drawing VBL on this thread */
    op_time_t tick_ema, tick_dev;     /* execution time of a drawing VBL (the tick) */
    op_time_t tick_late;              /* the last tick's start past its pacer deadline, when a stall */
    int       span_late;              /* the open span was planned after a stalled tick start */
    op_time_t tick_peak;              /* decaying peak of the tick's execution time */
    op_time_t lag;                    /* animation lag behind the display instant */
    op_time_t lag_pin;                /* OPENPETE_EAGER_LAG_US */
    int       lag_headroom;
    int       dbg_level;
    unsigned  trimmed;                /* spans whose render-grid slot was pulled back to within a period of now */
    op_time_t anchor_age;             /* now - the span anchor at the last plan: 0 when anchored on now */
    op_time_t last_target;            /* the previous span's final display instant */
    int       world_live;             /* canonical presents are flowing (see the inline gate) */
    unsigned  spans, produced, skipped_late, cut, refused, level_up, level_down;
    unsigned  span_held;              /* spans planned above the standing level for an isolated shortfall */
    unsigned  produced_inter;         /* of `produced`, the sub-tick re-extracts */
    int       span_cut;               /* this span ended with targets unproduced */
    op_eager_cut_t span_cause;        /* why: set with span_cut, read by the next plan */
    op_eager_cut_t stop;              /* why the produce walk last stopped short of the open span's plan */
    unsigned  cut_deadline, cut_ring; /* cut spans by cause, of `cut` */
    op_eager_count_t count;           /* the free span's count controller */
    int       span_free;              /* the open span was planned with no display grid */
    op_eager_count_t fill_count;      /* the fill cadence's count controller on a display grid */
    int       span_fill;              /* the open span was planned by the fill cadence on a display grid */
    int       fill_afford;            /* the open fill span's afford, at most its slot count */
    int       fill_k;                 /* the open fill span's planned scenes */
    unsigned  fill_spans;             /* spans planned by the fill cadence */
    int       span_pub;               /* scenes of the open span published into the scene ring */
    int       span_afford;            /* the open span's afford */
    int       span_made;              /* scenes the open span produced */
    int       span_seed;              /* scenes the open free span's count controller planned */
    int       replan_on;              /* OPENPETE_EAGER_REPLAN (default 1): re-plan a free span after its update VBL */
    int       span_replanned;         /* the re-plan changed the open span's planned count */
    unsigned  replanned;              /* spans whose planned count the re-plan changed */
    op_time_t span_grid;              /* the open free span's render grid period, its spacing floor's first term */
    int       closed_n, closed_spare, closed_capacity;   /* the span closed last: final plan, spare, made + spare */
    int       spare;                  /* scenes the guard would still have started when the open span's plan finished */
    op_time_t span_len;               /* the open free span's length, anchor to last instant */
    op_time_t produce_end;            /* the open span's update-VBL sim deadline */
    unsigned  canon_frame;            /* openpete_frame at the last canonical, 0 after a discontinuity */
} s_eager = { .ladder = { .level = 1, .since_short = EAGER_HEADROOM_RUNS }, .level = 1,
             .count_spare = EAGER_COUNT_SPARE, .count_runs = EAGER_COUNT_RUNS, .replan_on = 1 };


/** @brief Book the interval since the previous plan to its arm, and switch arms at a window boundary (OPENPETE_EAGER_CAP_AB).
 *
 * Runs at each plan, before the cap is computed, so the span about to be
 * planned runs under the arm this call leaves in s_eager.cap_on.
 * @param now The plan instant. */
static void eager_cap_ab_step(op_time_t now)
{
    if (s_eager.ab.window <= 0) return;
    const unsigned pres = openpete_present_thread_presents();
    const unsigned sup = openpete_native_scene_ring_superseded();
    if (s_eager.ab.counting && s_eager.world_live && s_eager.ab.t0) {
        eager_cap_arm_t *a = &s_eager.ab.acc[s_eager.ab.arm];
        a->ns += now - s_eager.ab.t0;
        a->presents += pres - s_eager.ab.pres0;
        a->sup += sup - s_eager.ab.sup0;
        a->spans++;
        a->capped += (unsigned)s_eager.span_capped;
    }
    s_eager.ab.t0 = now; s_eager.ab.pres0 = pres; s_eager.ab.sup0 = sup;
    if (!s_eager.ab.arm_start || now - s_eager.ab.arm_start >= s_eager.ab.window) {
        s_eager.ab.arm ^= 1;
        s_eager.ab.arm_start = now;
        s_eager.cap_on = s_eager.ab.null_ab ? s_eager.ab.cap_cfg : s_eager.ab.arm;
    }
    s_eager.ab.counting = now - s_eager.ab.arm_start >= s_eager.ab.window / 4;
}

/** @brief Log the presenter cap A/B's per-arm presents per second, retired scenes per span and capped spans. */
static void eager_cap_ab_report(void)
{
    if (s_eager.ab.window <= 0) return;
    for (int k = 0; k < 2; k++) {
        const eager_cap_arm_t *a = &s_eager.ab.acc[k];
        OPL_INFO_F("eager", "cap-ab arm%d (%s): %.3f s counted, %u span(s), %u present(s) = %.2f/s, "
                   "%u superseded = %.3f per span, %u capped span(s)",
                   k, s_eager.ab.null_ab ? (s_eager.ab.cap_cfg ? "null, cap on" : "null, cap off") : (k ? "cap on" : "cap off"),
                   (double)a->ns / 1e9, a->spans, a->presents, a->ns > 0 ? a->presents * 1e9 / (double)a->ns : 0.0,
                   a->sup, a->spans ? (double)a->sup / a->spans : 0.0, a->capped);
    }
}

/** @brief Log the presenter cap's state and backlog gate: on or off, spans capped, armings, disarmings, slack, and the presenter's service time and picks. */
static void eager_cap_report(void)
{
    unsigned picks, starved;
    openpete_present_thread_picks(&picks, &starved);
    OPL_INFO_F("eager", "presenter cap %s: %u span(s) capped; backlog gate %s, %u arming(s), %u disarming(s), "
               "%u block(s) raised the slack (now %d), next arming needs %d span(s); "
               "service %.3f ms ema; %u pick(s), %u starved",
               s_eager.cap_on ? "on" : "off", s_eager.capped_spans, s_eager.cap.armed ? "armed" : "disarmed",
               s_eager.cap.arms, s_eager.cap.disarms, s_eager.cap.loosened, s_eager.cap.slack,
               s_eager.cap.need > 0 ? s_eager.cap.need : EAGER_CAP_ARM_SPANS,
               (double)openpete_present_thread_service_ns() / 1e6, picks, starved);
    eager_cap_ab_report();
}

/* Documented at the declaration in eager.h. */
int eager_active(void) { return s_eager.active; }

/* Documented at the declaration in eager.h. */
int eager_world_live(void) { return s_eager.world_live; }

/* Documented at the declaration in eager.h. */
void eager_set_active(int on)
{
    if (on == s_eager.active) return;
    openpete_present_thread_set_active(on);   /* 0: drains */
    s_eager.active = on;
    OPL_INFO_F("eager", "%s", on ? "engaged" : "disengaged (inline presents)");
}

/* Documented at the declaration in eager.h. */
int eager_hold_done(void)
{
    static int armed = -1;
    static op_time_t until;
    if (armed < 0) {
        const char* e = getenv("OPENPETE_EAGER_HOLD_S");
        armed = (e && *e) ? 1 : 0;
        until = op_now() + (op_time_t)(armed ? atof(e) * 1e9 : 0); /* clock-guard: wall */
        if (armed) OPL_INFO_F("eager", "held off for %.3f s (OPENPETE_EAGER_HOLD_S)", atof(e));
    }
    return !armed || op_now() >= until; /* clock-guard: wall */
}
/** @brief Read the OPENPETE_EAGER_* tuning variables into s_eager, once.
 *
 * OPENPETE_EAGER_POLICY=greedy selects policy 1 (evenly spaced targets),
 * else policy 0 (every level-th grid slot). OPENPETE_EAGER_LEVEL, when
 * nonzero, disables the adaptive level ladder, and a positive value also
 * sets the level. OPENPETE_EAGER_AFFORD pins the per-span scene budget and
 * OPENPETE_EAGER_LAG_US (microseconds) pins the lag. OPENPETE_EAGER_CAP=0
 * turns off the presenter cap (eager_cap_afford). OPENPETE_EAGER_COUNT_SPARE
 * (scenes, default EAGER_COUNT_SPARE) sets the room a span must leave to
 * count as clean for the free-span and fill count controllers, and
 * OPENPETE_EAGER_COUNT_RUNS (spans, default EAGER_COUNT_RUNS) the free
 * span's clean run that gives back one scene. OPENPETE_EAGER_REWARM=0
 * turns off the re-warm window at the world going live (eager_note_live).
 * OPENPETE_EAGER_REPLAN=0 turns off the free span's re-plan after its
 * update VBL (eager_replan). Later calls do nothing. */
static void eager_init(void) OP_REQUIRES(sim)
{
    static int inited;
    if (inited) return;
    inited = 1;
    const char *p = getenv("OPENPETE_EAGER_POLICY");
    s_eager.policy = (p && !strcmp(p, "greedy")) ? 1 : 0;
    const char *l = getenv("OPENPETE_EAGER_LEVEL");
    s_eager.pinned = (l && *l) ? atoi(l) : 0;
    if (s_eager.pinned > 0) s_eager.level = s_eager.ladder.level = s_eager.pinned;
    const char *f = getenv("OPENPETE_EAGER_AFFORD");
    s_eager.afford_pin = (f && *f) ? atoi(f) : 0;
    const char *cap = getenv("OPENPETE_EAGER_CAP");
    s_eager.cap_on = !(cap && *cap == '0');
    if (!s_eager.cap_on) OPL_INFO_F("eager", "%s", "presenter cap off (OPENPETE_EAGER_CAP=0)");
    const char *ab = getenv("OPENPETE_EAGER_CAP_AB");
    s_eager.ab.window = (ab && *ab) ? (op_time_t)atoll(ab) * 1000000 : 0;
    const char *abn = getenv("OPENPETE_EAGER_CAP_AB_NULL");
    s_eager.ab.null_ab = abn && *abn && *abn != '0';
    s_eager.ab.cap_cfg = s_eager.cap_on;
    const char *g = getenv("OPENPETE_EAGER_LAG_US");
    s_eager.lag_pin = (g && *g) ? (op_time_t)atoll(g) * 1000 : 0;
    const char *rw = getenv("OPENPETE_EAGER_REWARM");
    s_eager.rewarm_on = !(rw && *rw == '0');
    const char *re = getenv("OPENPETE_EAGER_REWARM_EDGE");
    s_eager.rewarm_edge_on = re && *re && *re != '0';
    s_eager.edges_seen = openpete_native_level_edges();
    const char *cs = getenv("OPENPETE_EAGER_COUNT_SPARE");
    if (cs && *cs && atoi(cs) >= 0) s_eager.count_spare = atoi(cs);
    const char *cr = getenv("OPENPETE_EAGER_COUNT_RUNS");
    if (cr && *cr && atoi(cr) >= 1) s_eager.count_runs = atoi(cr);
    const char *rp = getenv("OPENPETE_EAGER_REPLAN");
    s_eager.replan_on = !(rp && *rp == '0');
    if (!s_eager.replan_on) OPL_INFO_F("eager", "free-span re-plan off (OPENPETE_EAGER_REPLAN=0)");
    if (s_eager.count_spare != EAGER_COUNT_SPARE || s_eager.count_runs != EAGER_COUNT_RUNS)
        OPL_INFO_F("eager", "count controller: spare %d scene(s), clean run %d span(s)",
                   s_eager.count_spare, s_eager.count_runs);
}

/** @brief Return the first instant of the lattice v(n) = base + n * period at or after t.
 *
 * An instant up to a millionth of a period before t counts as at t, so a t
 * already on the lattice does not round up a whole period.
 * @param t Time on the op_now() clock.
 * @param base Time of lattice instant 0.
 * @param period Lattice period in nanoseconds.
 * @return The lattice instant in nanoseconds. */
static op_time_t eager_lattice_ceil(op_time_t t, int64_t base, double period)
{
    const double n = ceil((double)(t - base) / period - 1e-6);
    return base + (op_time_t)(n * period + 0.5);
}

/** @brief Return the tail bound of one eager scene's production cost.
 *
 * The mean plus three deviations of the per-scene production time that
 * eager_produce_one measures. eager_produce stops before a scene whose tail
 * cost would pass its deadline, and eager_plan floors the render-grid
 * period at it when presenting is uncapped.
 * @return The bound in nanoseconds, or 0 before EAGER_COST_WARMUP scenes
 *         have been measured. */
static op_time_t eager_tail_cost(void)
{
    return s_eager.cost_n >= EAGER_COST_WARMUP
        ? s_eager.cost_ema + 3 * s_eager.cost_dev : 0;
}

/* The plan's budget keeps this much of the span unplanned against estimate
 * error; lateness beyond it is lateness the produce guard absorbs. */
#define EAGER_BUDGET_SAFETY_NS 1000000

/* Documented at the declaration in eager.h. */
void eager_note_vbl(op_time_t ex, op_time_t start_late, int drawing)
{
    const int stall = ex >= (op_time_t)g_openpete_tick_period_ns / 2;
    if (!drawing) {
        if (!stall) s_eager.vbl_ema = s_eager.vbl_ema ? (s_eager.vbl_ema * 7 + ex) / 8 : ex;
        return;
    }
    s_eager.tick_late = start_late >= (op_time_t)g_openpete_tick_period_ns / 2 ? start_late : 0;
    if (stall) return;
    if (s_eager.tick_ema) {
        const op_time_t d = ex > s_eager.tick_ema ? ex - s_eager.tick_ema : s_eager.tick_ema - ex;
        s_eager.tick_dev = (s_eager.tick_dev * 7 + d) / 8;
    }
    s_eager.tick_ema = s_eager.tick_ema ? (s_eager.tick_ema * 7 + ex) / 8 : ex;
    if (ex > s_eager.tick_peak) s_eager.tick_peak = ex;
}

/** @brief Record one intermediate scene the eager producer dropped as a SHED record.
 *
 * The record is the one the inline path writes for a present it does not
 * make: kind OP_PACE_INTERMEDIATE, the per-scene tail cost as the predicted
 * cost, and the time left before the scene's deadline as the margin.
 * @param reason PACE_SHED_NO_BUDGET or PACE_SHED_INSTANT_PASSED.
 * @param margin_ns Time left before the deadline; negative once it passed. */
static void eager_shed(uint16_t reason, op_time_t margin_ns)
{
    openpete_pace_shed(OP_PACE_INTERMEDIATE, reason,
                       (int64_t)eager_tail_cost(), (int64_t)margin_ns);
}

/** @brief Close the previous span: count it cut when targets were left unproduced, name the cause, and shed each one.
 *
 * Every target from s_eager.i to s_eager.n is an intermediate scene (the
 * canonical scene, index 0, is produced when its span is planned) that the
 * produce guard held back for the deadline or a full scene ring, and the new
 * plan discards it. The cause (span_cause) is LATE for a span planned after
 * a stalled tick start, WITHHELD while the world is not live or when the
 * walk never stopped short (production was not resumed), else the walk's
 * last stop: DEADLINE or RING. Each unproduced target is written as one
 * PACE_SHED_NO_BUDGET record while the world is live; while it is not, the
 * display holds the last image, which is not a shed. Sets span_cut for the
 * plan's level ladder. Keeps the span's final planned count, its spare and
 * its capacity (scenes produced plus spare; 0 when the plan finished before
 * the update VBL, where spare is not measured) for the next plan's records. */
static void eager_close_span(void)
{
    s_eager.span_cause = OP_EAGER_CUT_NONE;
    s_eager.closed_n = s_eager.n;
    s_eager.closed_spare = s_eager.spare;
    s_eager.closed_capacity = s_eager.spare < OP_EAGER_LADDER_MAX_AFFORD ? s_eager.span_made + s_eager.spare : 0;
    if (!s_eager.n || s_eager.i >= s_eager.n) return;
    s_eager.cut++;
    s_eager.span_cut = 1;
    s_eager.span_cause = s_eager.span_late ? OP_EAGER_CUT_LATE
                       : !s_eager.world_live || s_eager.stop == OP_EAGER_CUT_NONE ? OP_EAGER_CUT_WITHHELD
                       : s_eager.stop;
    s_eager.cut_deadline += s_eager.span_cause == OP_EAGER_CUT_DEADLINE;
    s_eager.cut_ring += s_eager.span_cause == OP_EAGER_CUT_RING;
    if (!s_eager.world_live) return;
    for (int q = s_eager.i; q < s_eager.n; q++) eager_shed(PACE_SHED_NO_BUDGET, 0);
}

/** @brief Shed the display-grid slots of a new span that the plan leaves without a scene for cost.
 *
 * Two plan verdicts leave a slot of the display grid unproduced because
 * scenes cost more than the span affords: the cost floor that coarsens the
 * grid period to the scene or present-thread cost, and the decimation level
 * (or the greedy policy's afford) that targets fewer slots than the grid
 * holds. Each such slot is the eager form of the inline present the host
 * loop sheds with PACE_SHED_NO_BUDGET, and is written as one. A level pinned
 * by OPENPETE_EAGER_LEVEL under the divisor policy is a configuration, not a
 * cost verdict, and sheds nothing; nor does any slot while the world is not
 * live, when no scene is produced and the display holds the last image.
 * @param first The span's first display instant.
 * @param span_end The span's last admissible display instant.
 * @param grid_period The display grid period before the cost floor.
 * @param period The planned grid period, at least grid_period.
 * @param ns The number of slots enumerated at period. */
static void eager_shed_unplanned(op_time_t first, op_time_t span_end,
                                 op_time_t grid_period, op_time_t period, int ns)
{
    if (!s_eager.world_live) return;
    int grid_slots = ns;
    if (grid_period > 0 && grid_period < period && span_end >= first) {
        const op_time_t g = (span_end - first) / grid_period + 1;
        grid_slots = g < EAGER_MAX_TARGETS ? (int)g : EAGER_MAX_TARGETS;
    }
    const int ladder = s_eager.pinned == 0 || s_eager.policy == 1;
    const int planned = ladder ? s_eager.n : ns;
    for (int q = planned; q < grid_slots; q++) eager_shed(PACE_SHED_NO_BUDGET, 0);
}

/* Room for the canonical scene between its plan and its vblank: production
 * (~0.25 ms), compose (0.3 ms on a light scene, 1.2 ms on a 48k-prim vista)
 * and the driver's own lead to the flip (a submit 0.33 ms before a vblank
 * still made it; one 0.13 ms before did not). */
#define EAGER_PUBLISH_LEAD_NS 2000000
/* The present thread's fitted vblank lattice, when targets go on it. */
typedef struct { int on; int64_t base; double period; } eager_lattice_t;
/* One span's display instants, as eager_span_slots enumerates them; prev is
 * the old grid's instant one period before first when this span continues
 * the previous span's grid (op_eager_span_continue_in), else 0. */
typedef struct { op_time_t first, end, prev; int ns; op_time_t slots[EAGER_MAX_TARGETS]; } eager_span_t;

/** @brief Enumerate the display instants of one span from its first render-grid slot.
 *
 * Raises span_end to first, puts first on the vblank lattice (no earlier than
 * EAGER_PUBLISH_LEAD_NS past now) when one is fitted, extends span_end by the
 * lag and, when the previous span's last target plus a period lies past
 * first, continues on the previous span's grid after that target, keeping
 * only its phase when it lies past both the extended span_end and a period
 * past first (op_eager_span_continue_in), and records the old grid's
 * instant before first as the span's prev. The
 * slots step period from first through span_end; a span holds at least the
 * slot at first. Reads s_eager.lag and s_eager.last_target and writes
 * nothing else.
 * @param now The plan instant.
 * @param first The span's first render-grid slot.
 * @param span_end The instant the next tick lands.
 * @param period The planned grid period.
 * @param lat The vblank lattice.
 * @param out Receives first, the extended span_end, the slot count and the slots. */
static void eager_span_slots(op_time_t now, op_time_t first, op_time_t span_end,
                             op_time_t period, const eager_lattice_t *lat, eager_span_t *out)
{
    if (span_end < first) span_end = first;
    if (lat->on) {
        /* The canonical scene is produced right after this plan and
         * shown at once; its vblank must leave room for that. */
        op_time_t f = now + EAGER_PUBLISH_LEAD_NS;
        if (f < first) f = first;
        first = eager_lattice_ceil(f, lat->base, lat->period);
    }
    /* Display instants whose animation instant (t - lag) is inside the span.
     * The previous span already covers the instants up to its last target
     * (that is what the lag is for: its late scenes show through this tick's
     * execution), so this span begins at the slot after it -- a scene for an
     * instant the old span covers would sit at negative alpha of the new
     * bracket, a backward extrapolation the old bracket renders exactly. */
    /* The extension below moves the SLOT ENUMERATION past the instant
     * production stops at, which is correct - those late instants are shown
     * through the next tick's execution - but it is not time in which to
     * produce them. eager_plan's `prod_end` is that separate quantity. */
    span_end += s_eager.lag;
    out->prev = 0;
    if (s_eager.last_target && s_eager.last_target + period > first) {
        /* the old span's grid after its last target; its phase only once it ran past this span */
        op_time_t f = op_eager_span_continue_in(s_eager.last_target, first, span_end, period, &out->prev);
        /* re-snapped: the lattice fit moves by microseconds between spans */
        if (lat->on) f = eager_lattice_ceil(f - period / 2, lat->base, lat->period);
        first = f;
    }
    int ns = 0;
    for (op_time_t t = first; t <= span_end && ns < EAGER_MAX_TARGETS; t += period) out->slots[ns++] = t;
    if (ns == 0) out->slots[ns++] = first;
    out->first = first; out->end = span_end; out->ns = ns;
}

/** @brief Write eager_plan's OPENPETE_EAGER_DBG lines for the span just planned.
 *
 * OPENPETE_EAGER_DBG=1 writes the plan line on a cut span, a change of the
 * span's level, a span planned above the standing level (std=), or a tick
 * that started half a tick or more past its deadline (late=). Of the
 * previous span: cut=, cause= (deadline, ring, withheld, late, or - when
 * produced), made= (scenes produced / planned) and spare=; of the free
 * span's count controller after its step: keep= (the fraction of afford
 * planned) and drain= (the ring cut's spacing floor, 0 when none); cad=, the
 * cadence policy (even or fill).
 * 2 or more also writes one span line per plan: the offsets from now of the
 * render-grid slot passed in (in), the span's first display instant, its end
 * after the lag extension and the previous span's last target, the lag, the
 * anchor source (cap: the newest capture stamp, now: the plan instant) with
 * the capture stamp's age, the slot count, the level and the count of spans
 * whose slot was pulled back. 3 or more also writes the slot list beside the
 * one the untrimmed slot and the caller-side span_end clamp enumerate, and
 * whether the two are equal. Runs before the plan stores its targets, so
 * last_target is still the previous span's.
 * @param now The plan instant.
 * @param first_in The render-grid slot eager_plan was called with.
 * @param end_in The span_end eager_plan was called with.
 * @param period The planned grid period.
 * @param lat The vblank lattice.
 * @param sp The planned span.
 * @param budget The production budget in nanoseconds.
 * @param afford The number of scenes the budget affords. */
static void eager_plan_dbg(op_time_t now, op_time_t first_in, op_time_t end_in, op_time_t period,
                           const eager_lattice_t *lat, const eager_span_t *sp,
                           op_time_t budget, int afford)
{
    static int dbg = -1;
    if (dbg < 0) { const char *e = getenv("OPENPETE_EAGER_DBG"); dbg = (e && *e) ? atoi(e) : 0; }
    if (dbg <= 0) return;
    if (s_eager.span_cut || s_eager.level != s_eager.dbg_level || s_eager.level != s_eager.ladder.level ||
        s_eager.tick_late > 0) {
        static const char *const cause[] = { "-", "deadline", "ring", "withheld", "late" };
        fprintf(stderr, "[eager f=%u] plan: slots=%d budget=%.2fms tail=%.3fms afford=%d level=%d std=%d keep=%.3f "
                "cut=%d cause=%s made=%d/%d spare=%d drain=%.3fms lag=%.2fms vbl=%.2fms late=%.2fms cad=%s\n",
                openpete_frame, sp->ns, (double)budget / 1e6, (double)eager_tail_cost() / 1e6, afford,
                s_eager.level, s_eager.ladder.level,
                1.0 - (double)s_eager.count.deficit / OP_EAGER_COUNT_ONE, s_eager.span_cut,
                cause[s_eager.span_cause], s_eager.span_made, s_eager.n, s_eager.spare,
                (double)op_eager_count_floor(&s_eager.count) / 1e6, (double)s_eager.lag / 1e6,
                (double)s_eager.vbl_ema / 1e6, (double)s_eager.tick_late / 1e6,
                openpete_get_cadence() == OPENPETE_CADENCE_FILL ? "fill" : "even");
        s_eager.dbg_level = s_eager.level;
    }
    if (dbg < 2) return;
    const double last = s_eager.last_target ? (double)(s_eager.last_target - now) / 1e6 : 0.0;
    const op_time_t curr = openpete_interp_curr_tick_ns();
    const double age = curr ? (double)(now - curr) / 1e6 : -1.0;
    fprintf(stderr, "[eager f=%u] span: now=%.6f in=%.3f first=%.3f end=%.3f last=%.3f lag=%.3f anchor=%s age=%.3f ns=%d level=%d trimmed=%u\n",
            openpete_frame, (double)now / 1e9, (double)(first_in - now) / 1e6,
            (double)(sp->first - now) / 1e6, (double)(sp->end - now) / 1e6, last,
            (double)s_eager.lag / 1e6, s_eager.anchor_age ? "cap" : "now",
            age, sp->ns, s_eager.level, s_eager.trimmed);
    if (dbg < 3) return;
    eager_span_t old;
    eager_span_slots(now, first_in, end_in > first_in ? end_in : first_in, period, lat, &old);
    const int same = old.ns == sp->ns && old.first == sp->first && old.end == sp->end &&
                     memcmp(old.slots, sp->slots, sizeof(op_time_t) * (size_t)sp->ns) == 0;
    char buf[1024]; int o = snprintf(buf, sizeof(buf), "[eager f=%u] slots: same=%d new", openpete_frame, same);
    for (int q = 0; q < sp->ns && o < 900; q++) o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %.3f", (double)(sp->slots[q] - now) / 1e6);
    o += snprintf(buf + o, sizeof(buf) - (size_t)o, " | old");
    for (int q = 0; q < old.ns && o < 1000; q++) o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %.3f", (double)(old.slots[q] - now) / 1e6);
    fprintf(stderr, "%s\n", buf);
}

/** @brief Return how many scenes the span's production budget affords.
 *
 * The budget runs from now, less the start lateness a stall left the tick
 * (tick_late), to prod_end, less the update VBL's mean cost and
 * EAGER_BUDGET_SAFETY_NS. A stall's lateness is not capacity: the produce
 * guard stops a late span at its deadline, and a budget priced from now
 * would read one load as the machine affording one scene, raising the
 * level to the slot count. The tick's own execution stays in the
 * budget, so a run of long ticks still prices the spans it shortens. Past
 * warmup the scenes are priced on their sum, N times the mean cost plus two
 * deviations of the sum, 2 * sqrt(N) deviations (op_eager_ladder_afford);
 * before it, on the per-scene tail, or one per slot before any cost exists.
 * OPENPETE_EAGER_AFFORD overrides the result, which is at least 1.
 * @param now The plan instant.
 * @param prod_end The pacer grid instant production must finish before.
 * @param ns The span's slot count.
 * @param budget_out Receives the budget in nanoseconds, floored at 0.
 * @return The number of scenes afforded. */
static int eager_afford(op_time_t now, op_time_t prod_end, int ns, op_time_t *budget_out)
{
    /* How many scenes fit is a question about their SUM, not about one of
     * them. `eager_tail_cost()` is a per-scene p99.7 bound, which is the right
     * question between scenes (the produce guard) and the wrong one here:
     * pricing all N at once at p99.7 charges every scene the worst scene's
     * cost, ~2x the true sum, since independent costs average out. The sum's
     * own spread grows as sqrt(N), so it is bounded at the N being priced: a
     * constant sized for sixteen scenes reserves four deviations more than
     * two scenes spread, which at a scene cost near half the budget prices
     * one scene where two fit. Being wrong is also cheap in one direction
     * only: the produce guard still stops at the deadline, and a cut span is
     * observed and answered by the ladder. */
    const op_time_t tail = eager_tail_cost();
    op_time_t budget = prod_end - (now - s_eager.tick_late) - s_eager.vbl_ema - EAGER_BUDGET_SAFETY_NS;
    if (budget < 0) budget = 0;
    *budget_out = budget;
    int afford;
    if (s_eager.cost_n >= EAGER_COST_WARMUP && s_eager.cost_ema > 0) {
        afford = op_eager_ladder_afford(budget, s_eager.cost_ema, s_eager.cost_dev);
    } else {
        afford = tail > 0 ? (int)(budget / tail) : ns;
    }
    if (s_eager.afford_pin > 0) afford = s_eager.afford_pin;   /* a forced budget, for the policy A/B */
    return afford < 1 ? 1 : afford;
}

/** @brief Move the standing decimation level for a span of ns slots of which afford scenes fit, and set the span's level.
 *
 * op_eager_ladder_step decides: a span that needs more than the standing
 * level, or follows a cut span, plans at the level it needs; the standing
 * level rises only when such a shortfall recurs within EAGER_HEADROOM_RUNS
 * spans, and falls after EAGER_HEADROOM_RUNS consecutive clean spans, in one
 * move, to the highest level any of them needed. A cut span planned after a
 * stalled tick start (span_late) is not a shortfall: the produce guard
 * absorbed the stall at the deadline, which says nothing about what the
 * steady span affords. Counts standing rises in level_down, falls in
 * level_up, and spans planned above the standing level in span_held.
 * @param ns The span's slot count.
 * @param afford The scenes the budget affords, at least 1. */
static void eager_ladder_step(int ns, int afford)
{
    const int before = s_eager.ladder.level;
    const int cut = s_eager.span_cut && !s_eager.span_late;
    s_eager.level = op_eager_ladder_step(&s_eager.ladder, ns, afford, cut, EAGER_HEADROOM_RUNS);
    if (s_eager.ladder.level > before) s_eager.level_down++;
    else if (s_eager.ladder.level < before) s_eager.level_up++;
    if (s_eager.level > s_eager.ladder.level) s_eager.span_held++;
}

/** @brief Move the animation lag toward the tick's execution tail, unless OPENPETE_EAGER_LAG_US pins it.
 *
 * Decays the tick-time peak 1/32 toward the tick mean, then raises the lag
 * at once to the peak plus 0.3 ms, or lowers it to that value only after 30
 * consecutive spans that wanted less. */
static void eager_lag_update(void)
{
    /* The lag follows the tick's execution tail: up at once (a slot at
     * alpha > 1 is a lost frame), down only after a second of headroom (a
     * lag that flickers warps animation time). */
    if (s_eager.lag_pin > 0) s_eager.lag = s_eager.lag_pin;
    else {
        /* The peak over roughly the last second, decaying toward the centre:
         * a tail bound that a rare long tick raises at once and that lets go
         * of it again, unlike a deviation term that a bimodal tick inflates
         * for good. */
        s_eager.tick_peak -= (s_eager.tick_peak - s_eager.tick_ema) / 32;
        const op_time_t want = s_eager.tick_peak + 300000;
        if (want > s_eager.lag) { s_eager.lag = want; s_eager.lag_headroom = 0; }
        else if (++s_eager.lag_headroom >= 30) { s_eager.lag = want; s_eager.lag_headroom = 0; }
    }
}

/** @brief Return the planned grid period: the render period moved onto the display's refresh grid, floored at the production cost.
 *
 * On the present thread's locked vblank lattice the period is a whole
 * number of refreshes; otherwise, on a vsynced display slower than the
 * render grid, a whole number of render slots per refresh. The result never
 * falls below the scene production tail cost (250 us before it is
 * measured) or the present thread's cost. Without vsync there is no display
 * grid: the span is free, and eager_span_fill re-spaces its slots once the
 * budget is known.
 * @param period The render grid period in nanoseconds.
 * @param lat The vblank lattice.
 * @param grid_period Receives the display grid period before the cost floor.
 * @param free_grid Receives nonzero when no display grid constrains the span's instants.
 * @return The planned grid period in nanoseconds. */
static op_time_t eager_grid_period(op_time_t period, const eager_lattice_t *lat, op_time_t *grid_period,
                                   int *free_grid)
{
    const float hz = openpete_native_display_refresh_hz();
    if (lat->on) {
        /* The present thread has fitted the display's vblank lattice
         * from observed flips: targets go ON it, a whole number of
         * refreshes apart, so each target is a vblank and the
         * animation instant (target - lag) is exact. The nominal mode
         * rate is not good enough for this -- it was 2e-5 off the
         * measured one, a full slot of phase every ~50 s. */
        const op_time_t lp = (op_time_t)(lat->period + 0.5);
        op_time_t k = (period + lp / 2) / lp;             /* refreshes per render slot */
        if (k < 1) k = 1;
        period = (op_time_t)(lat->period * (double)k + 0.5);
    } else
    if (hz > 1.0f && openpete_native_display_vsync()) {
        const op_time_t rp = (op_time_t)(1e9 / hz);
        if (rp > period + period / 8) {
            const op_time_t k = (rp + period / 2) / period;   /* whole slots per refresh */
            if (k > 1) period *= k;
        }
    }
    /* Uncapped presenting (vsync off, --render-fps far above the display):
     * targets denser than scenes can be made would plan a span the ring
     * cannot hold and the burst cannot finish. Floor the grid at the
     * production cost; the ring's back-pressure below does the rest, and
     * the present thread shows the newest scene at whatever rate the GPU
     * allows. This enumeration is provisional for a free span: its slot
     * count prices the budget before the cost is measured and its prev
     * anchors eager_span_fill. */
    *free_grid = !lat->on && !openpete_native_display_vsync();
    *grid_period = period;
    op_time_t floor_ns = eager_tail_cost() ? eager_tail_cost() : 250000;
    const op_time_t pc = openpete_present_thread_cost_ns();
    if (pc > floor_ns) floor_ns = pc;
    if (period < floor_ns) period = floor_ns;
    return period;
}

/** @brief Return the least spacing of a free span's instants: the largest of the render grid's period, the present thread's cost, the ring cut's drain floor and, when asked, the presenter cap's spacing.
 *
 * The render grid's period is the --render-fps cap; the present thread's
 * measured cost and the drain floor a recent ring cut set
 * (op_eager_count_floor) bound how fast scenes can be shown. The seed plan
 * (eager_span_fill) and the re-plan after the update VBL (eager_replan)
 * both take their floor here, so a further bound on the present side's
 * rate added to this function applies to both. The presenter cap's spacing
 * (span_cap_ns, eager_cap_afford) is 0 unless the backlog gate is armed.
 * @param grid_period The render grid period in nanoseconds.
 * @param with_cap Nonzero to include the presenter cap's spacing.
 * @return The least spacing in nanoseconds. */
static op_time_t eager_free_floor(op_time_t grid_period, int with_cap)
{
    op_time_t floor_ns = openpete_present_thread_cost_ns();
    if (grid_period > floor_ns) floor_ns = grid_period;
    if (op_eager_count_floor(&s_eager.count) > floor_ns) floor_ns = op_eager_count_floor(&s_eager.count);
    if (with_cap && s_eager.span_cap_ns > floor_ns) floor_ns = s_eager.span_cap_ns;
    return floor_ns;
}

/** @brief Step the presenter cap's backlog gate on the present thread's picks, starved picks, busy time and the ring's retirements since the previous plan.
 *
 * Reads the deltas of openpete_present_thread_picks and
 * openpete_native_scene_ring_superseded, keeps the retirements in span_sup
 * for the EAGER_PLAN record, and steps the gate (op_eager_cap_step) while
 * the world is live, with the cap flag of the span those picks consumed,
 * the present thread's service time (openpete_present_thread_service_ns)
 * and the wall time since the previous plan: the gate's busy fraction is
 * the picks times service over the wall time of the same spans. Counts
 * the spans the cap lowered. Runs at each plan before eager_cap_afford.
 * @param now The plan instant on the op_now() clock. */
static void eager_cap_step(op_time_t now)
{
    unsigned picks, starved;
    openpete_present_thread_picks(&picks, &starved);
    const unsigned sup = openpete_native_scene_ring_superseded();
    const unsigned dp = picks - s_eager.picks_seen, dz = starved - s_eager.starved_seen;
    const op_time_t wall = s_eager.cap_plan_at > 0 ? now - s_eager.cap_plan_at : 0;
    s_eager.cap_plan_at = now;
    s_eager.span_sup = sup - s_eager.sup_seen;
    s_eager.picks_seen = picks; s_eager.starved_seen = starved; s_eager.sup_seen = sup;
    s_eager.capped_spans += (unsigned)s_eager.span_capped;
    if (s_eager.world_live)
        op_eager_cap_step(&s_eager.cap, dp, dz, s_eager.span_sup, s_eager.span_capped,
                          openpete_present_thread_service_ns(), wall, EAGER_CAP_ARM_SPANS, EAGER_CAP_ARM_MAX);
}

/** @brief Cap a span's afford at the scenes the present thread can consume over it, while the backlog gate is armed.
 *
 * The budget prices the sim thread's CPU only; a presenter slower than the
 * producer supersedes the excess unpresented. The cap
 * (op_eager_cap_count) is ceil(len / service) plus the gate's slack plus
 * one, len the span's presentation length from the previous span's last
 * target when this span continues it, else from its first slot, to its
 * lag-extended end; service is openpete_present_thread_service_ns. The cap
 * applies only while the gate (op_eager_cap_step) holds evidence that the
 * presenter is behind: a presenter that finds nothing due at most of its
 * picks is keeping up, and a cap would take scenes it can present. No cap applies
 * before the service time is measured, under OPENPETE_EAGER_AFFORD, or with
 * OPENPETE_EAGER_CAP=0. Sets span_cap (0 when none), span_cap_ns (a free
 * span's least spacing under the cap, read by eager_free_floor) and
 * span_capped.
 * @param afford The budget's afford, at least 1.
 * @param sp The span, enumerated by eager_span_slots.
 * @return The capped afford, at least 1. */
static int eager_cap_afford(int afford, const eager_span_t *sp)
{
    const op_time_t svc = openpete_present_thread_service_ns();
    s_eager.span_cap = 0;
    s_eager.span_cap_ns = 0;
    s_eager.span_capped = 0;
    if (!s_eager.cap_on || !s_eager.cap.armed || s_eager.afford_pin > 0 || svc <= 0) return afford;
    const op_time_t start = sp->prev ? sp->prev : sp->first;
    s_eager.span_cap = op_eager_cap_count(sp->end - start, svc, s_eager.cap.slack, EAGER_MAX_TARGETS,
                                          &s_eager.span_cap_ns);
    if (s_eager.span_cap >= afford) return afford;
    s_eager.span_capped = 1;
    return s_eager.span_cap;
}

/** @brief Re-space a free span's slots: as many as the budget affords, evenly over the span, and return their period.
 *
 * With no display grid the slot count is the planner's choice, and it is
 * afford, bounded by EAGER_MAX_TARGETS and by a least period
 * (eager_free_floor, the presenter cap's spacing included), so the ladder is asked for level 1 whenever the
 * estimate holds; a
 * grid sized from a different cost than the budget's would offer a slot or
 * two more than afford and halve the plan. A span
 * that continues the previous one (prev set) places its slots after prev
 * through the lag-extended end, so spans chain at one spacing; otherwise
 * the first slot stays at first. op_eager_ladder_fill does the arithmetic;
 * the span's length is kept in span_len for the drain floor and the grid
 * period in span_grid for the re-plan's floor. A span the presenter cap's
 * spacing plans fewer scenes for than the floor without it is marked
 * span_capped.
 * @param afford The scenes the budget affords, at least 1.
 * @param grid_period The render grid period in nanoseconds.
 * @param sp The span, enumerated by eager_span_slots; its first, slots and ns are rewritten.
 * @return The span's period in nanoseconds. */
static op_time_t eager_span_fill(int afford, op_time_t grid_period, eager_span_t *sp)
{
    const op_time_t start = sp->prev ? sp->prev : sp->first;
    const op_time_t floor_ns = eager_free_floor(grid_period, 1);
    const int a = afford < EAGER_MAX_TARGETS ? afford : EAGER_MAX_TARGETS;
    const int after = sp->prev != 0;
    int n;
    s_eager.span_len = sp->end - start;
    s_eager.span_grid = grid_period;
    const op_time_t p = op_eager_ladder_fill(s_eager.span_len, a, floor_ns, after, &n);
    if (s_eager.span_cap_ns > 0 && !s_eager.span_capped) {
        int n_free;
        (void)op_eager_ladder_fill(s_eager.span_len, a, eager_free_floor(grid_period, 0), after, &n_free);
        s_eager.span_capped = n < n_free;
    }
    sp->first = sp->prev ? sp->prev + p : sp->first;
    for (int q = 0; q < n; q++) sp->slots[q] = sp->first + p * q;
    sp->ns = n;
    return p;
}

/** @brief Plan a free span: step the count controller on the previous free span, re-space this one to its count, and set level 1.
 *
 * With no display grid there is no slot to skip, so the divisor ladder
 * does not run. The previous free span, closed while the world was live
 * and planned outside the re-warm window (span_warm), steps the count controller by its cut's cause (op_eager_count_step): a
 * deadline cut plans one scene of its afford fewer from here on, a ring cut
 * floors the spacing at the present thread's measured drain, a withheld or
 * late cut changes nothing, and a run of spans that finished with room
 * gives one scene back. The controller steps on the seed plan's view of
 * the span (op_eager_seed_view), since the re-plan after the update VBL
 * (eager_replan) sets the count the span finished with. The span plans
 * afford scaled by the controller's kept fraction, evenly spaced
 * (eager_span_fill); that count is the seed. A pinned level
 * (OPENPETE_EAGER_LEVEL) still decimates the filled slots.
 * @param afford The scenes the budget affords, at least 1.
 * @param grid_period The render grid period in nanoseconds.
 * @param sp The span, enumerated by eager_span_slots; its first, slots and ns are rewritten.
 * @return The span's period in nanoseconds. */
static op_time_t eager_free_span(int afford, op_time_t grid_period, eager_span_t *sp)
{
    if (s_eager.span_free && s_eager.world_live && !s_eager.span_warm) {
        int spare = 0;
        const op_eager_cut_t cause = op_eager_seed_view(s_eager.span_cause, s_eager.span_seed, s_eager.i,
                                                        s_eager.spare, &spare);
        op_eager_count_step(&s_eager.count, cause, s_eager.span_afford, spare,
                            s_eager.span_made, s_eager.span_len, s_eager.count_runs, s_eager.count_spare,
                            EAGER_DRAIN_RUNS);
    }
    if (!s_eager.pinned) s_eager.level = 1;
    return eager_span_fill(op_eager_count_plan(&s_eager.count, afford), grid_period, sp);
}

/** @brief Plan a span on a display grid under the fill cadence: step the grid's count controller and set the span's scene count.
 *
 * The previous fill span, closed while the world was live and planned
 * outside the re-warm window (span_warm), steps the controller (op_eager_fill_step: a deadline or ring cut plans one scene
 * fewer from here on, EAGER_HEADROOM_RUNS consecutive spans with
 * EAGER_COUNT_SPARE scenes of room give every one back). A span that
 * follows a span of another planner starts the controller from zero, since
 * the cuts it counted were of plans of another shape. The span plans
 * op_eager_fill_count scenes at level 1; eager_plan places them on the
 * slots with op_eager_spread_slot.
 * @param afford The scenes the budget affords, at least 1.
 * @param ns The span's slot count, at least 1. */
static void eager_fill_span(int afford, int ns)
{
    if (!s_eager.span_fill) memset(&s_eager.fill_count, 0, sizeof s_eager.fill_count);
    else if (s_eager.world_live && !s_eager.span_warm)
        op_eager_fill_step(&s_eager.fill_count, s_eager.span_cause, s_eager.fill_afford, s_eager.spare,
                           EAGER_HEADROOM_RUNS, s_eager.count_spare);
    s_eager.level = 1;
    s_eager.fill_afford = afford < ns ? afford : ns;
    s_eager.fill_k = op_eager_fill_count(&s_eager.fill_count, afford, ns);
}

/** @brief Choose the span's planner and run it: the free span's count, the fill cadence's count, or the divisor ladder.
 *
 * A span with no display grid is free (eager_free_span), whatever the
 * cadence. On a display grid the fill cadence (openpete_get_cadence, read
 * here at every plan) plans with eager_fill_span unless OPENPETE_EAGER_LEVEL
 * pins the level or OPENPETE_EAGER_POLICY=greedy chooses the instants, which
 * keep their own plans; otherwise the ladder steps (eager_ladder_step)
 * unless the level is pinned. Sets span_fill for this span.
 * @param free_grid Nonzero when no display grid constrains the span.
 * @param afford The scenes the budget affords, at least 1.
 * @param grid_period The render grid period in nanoseconds.
 * @param sp The span; a free span's slots are rewritten.
 * @param period The planned grid period in nanoseconds.
 * @return The span's period in nanoseconds: a free span's spacing, else period. */
static op_time_t eager_span_count(int free_grid, int afford, op_time_t grid_period, eager_span_t *sp,
                                  op_time_t period)
{
    const int fill = !free_grid && !s_eager.pinned && s_eager.policy == 0 &&
                     openpete_get_cadence() == OPENPETE_CADENCE_FILL;
    if (free_grid) period = eager_free_span(afford, grid_period, sp);
    else if (fill) eager_fill_span(afford, sp->ns);
    else if (!s_eager.pinned) eager_ladder_step(sp->ns, afford);
    s_eager.span_fill = fill;
    s_eager.fill_spans += (unsigned)fill;
    return period;
}

/** @brief Write the EAGER_PLAN pace record of the span just planned: the fields of the OPENPETE_EAGER_DBG plan line, on every span.
 *
 * Runs after the span's planner and before the previous span's books are
 * reset, so the record carries this span's plan (the FILL flag and the fill
 * count when the fill cadence planned it) beside the previous span's cut
 * flags, its published scene count, its spare, its capacity and its final
 * planned count (after any re-plan).
 * @param now The plan instant.
 * @param sp The planned span.
 * @param period The planned grid period.
 * @param budget The production budget in nanoseconds.
 * @param afford The number of scenes the budget affords. */
static void eager_plan_record(op_time_t now, const eager_span_t *sp, op_time_t period,
                              op_time_t budget, int afford)
{
    const int lvl = s_eager.level > 0 ? s_eager.level : 1;
    const int planned = s_eager.span_fill   ? s_eager.fill_k
                      : s_eager.policy == 1 ? (afford < sp->ns ? afford : sp->ns)
                                            : (sp->ns + lvl - 1) / lvl;
    const uint8_t flags = (uint8_t)((s_eager.span_cut ? OP_PACE_EP_CUT : 0u) |
                                    (s_eager.span_cause == OP_EAGER_CUT_RING ? OP_PACE_EP_RING : 0u) |
                                    (s_eager.span_cause == OP_EAGER_CUT_WITHHELD ? OP_PACE_EP_WITHHELD : 0u) |
                                    (s_eager.tick_late > 0 ? OP_PACE_EP_LATE : 0u) |
                                    (s_eager.pinned > 0 ? OP_PACE_EP_PINNED : 0u) |
                                    (s_eager.policy == 1 ? OP_PACE_EP_GREEDY : 0u) |
                                    (s_eager.span_fill ? OP_PACE_EP_FILL : 0u) |
                                    (openpete_interp_curr_is_dup() ? OP_PACE_EP_HEAD_DUP : 0u));
    const openpete_interp_snapshot_t *head = openpete_interp_curr_snapshot();
    openpete_pace_eager_plan(openpete_frame, now, flags, s_eager.level, s_eager.ladder.level, sp->ns,
                             afford, planned, s_eager.span_pub, period, budget, eager_tail_cost(), s_eager.lag,
                             s_eager.vbl_ema, s_eager.tick_late, s_eager.anchor_age,
                             head ? (int)(openpete_frame - head->tick) : -1,
                             s_eager.closed_spare, s_eager.closed_capacity, s_eager.closed_n,
                             s_eager.span_cap, s_eager.span_capped, s_eager.span_sup);
}

/* The scheduler's per-span state as profiler counters, in eager_prof_plan's order. */
static const char *const s_eager_prof_names[] = {
    "eager.cost_ema_ns", "eager.cost_dev_ns", "eager.tail_ns", "eager.budget_ns",
    "eager.afford", "eager.slots", "eager.planned", "eager.level", "eager.std_level",
    "eager.period_ns", "eager.grid_period_ns", "eager.free", "eager.lattice",
    "eager.warm", "eager.vbl_ema_ns", "eager.tick_late_ns", "eager.lag_ns",
    "pt.render_ema_ns", "pt.present_ema_ns", "eager.fill", "eager.prev_capacity", "eager.prev_planned"
};
#define EAGER_PROF_N ((int)(sizeof(s_eager_prof_names) / sizeof(s_eager_prof_names[0])))

/** @brief Emit the span plan's scheduler inputs and outputs as profiler COUNT records while a trace is open.
 *
 * One COUNT per name in s_eager_prof_names, raised on the sim thread after
 * the plan and before the canonical scene's SCENE marker, so each canonical
 * scene of the trace carries the plan it opened: the per-scene cost mean and
 * deviation and the tail bound built from them, the budget, the afford, the
 * slot count, the scenes planned, the span's and the standing level, the
 * planned and the display grid period, whether the span is free and whether
 * the vblank lattice was used, whether the cost estimate is past warmup, the
 * non-drawing VBL cost, the tick's start lateness, the lag, and the present
 * thread's render and present-step averages, whether the fill cadence
 * planned the span, and the previous span's capacity (scenes produced plus
 * the guard's room at its end, 0 when not measured) and final planned
 * count. It reads the scheduler's state
 * and writes none of it. Negative values are written as 0. Names are
 * interned on the first call made with a trace open.
 * @param budget The production budget in nanoseconds.
 * @param afford The scenes the budget affords.
 * @param ns The span's slot count.
 * @param period The planned grid period in nanoseconds.
 * @param grid_period The display grid period before the cost floor.
 * @param free_grid Nonzero when the span had no display grid.
 * @param lat_on Nonzero when the targets went on the vblank lattice. */
static void eager_prof_plan(op_time_t budget, int afford, int ns, op_time_t period,
                            op_time_t grid_period, int free_grid, int lat_on)
{
    if (!op_prof_on()) return;
    static int ids[EAGER_PROF_N], interned;
    if (!interned) {
        for (int q = 0; q < EAGER_PROF_N; q++) ids[q] = op_prof_intern(s_eager_prof_names[q]);
        interned = 1;
    }
    int64_t render = 0, present = 0;
    openpete_present_thread_emas(&render, &present);
    const int64_t v[EAGER_PROF_N] = {
        s_eager.cost_ema, s_eager.cost_dev, eager_tail_cost(), budget,
        afford, ns, s_eager.n, s_eager.level, s_eager.ladder.level,
        period, grid_period, free_grid != 0, lat_on != 0,
        s_eager.cost_n >= EAGER_COST_WARMUP, s_eager.vbl_ema, s_eager.tick_late, s_eager.lag,
        render, present, s_eager.span_fill, s_eager.closed_capacity, s_eager.closed_n
    };
    for (int q = 0; q < EAGER_PROF_N; q++) op_prof_count(ids[q], v[q] > 0 ? (uint64_t)v[q] : 0u);
}

/** @brief Plan the display instants of the span that starts now and adjust the decimation level.
 *
 * Counts the previous span as cut when it left targets unproduced. Pulls
 * first back by whole render periods to the grid's earliest instant at or
 * after now, counting each span it pulls back in s_eager.trimmed. Moves the
 * lag (unless pinned) up to the decaying tick-time peak plus 0.3 ms at once
 * and down only after 30 spans of headroom. Chooses the grid: on the
 * present thread's locked lattice a whole number of refreshes per render
 * slot with the first target on a vblank; otherwise, on a vsynced display
 * slower than the render grid, the refresh period; the period never falls
 * below the scene production cost, the present thread's cost, or 250 us
 * before the cost is measured. Enumerates the slots with eager_span_slots
 * (span_end raised to first and extended by the lag, continuing after the
 * previous span's last target). Estimates how many scenes fit
 * before prod_end (eager_afford), capped at the scenes the present thread
 * can consume over the span while its backlog gate holds (eager_cap_step,
 * eager_cap_afford). eager_span_count plans the count: without
 * vsync a free span (eager_free_span: afford less the count controller's
 * shortfall, evenly spaced, at level 1); on a display grid under the fill
 * cadence eager_fill_span's count; otherwise it moves the level
 * (eager_ladder_step). Records whether the span follows a stalled tick
 * start. Stores every level-th slot (policy 0), the fill count's slots
 * spread by op_eager_spread_slot (fill cadence), or afford evenly
 * spaced instants (policy 1) in s_eager.target, sets last_target and the
 * free span's seed count (span_seed; eager_replan may change the count of
 * unproduced instants after the update VBL, never last_target), writes the
 * profiler counters (eager_prof_plan) and counts the span.
 * @param now Current time on the op_now() clock.
 * @param first A render-grid instant at or after now.
 * @param period The render grid period in nanoseconds.
 * @param span_end The instant the next tick lands (alpha reaches 1).
 * @param prod_end The pacer grid instant production must finish before. */
static void eager_plan(op_time_t now, op_time_t first, op_time_t period,
                       op_time_t span_end, op_time_t prod_end)
{
    eager_close_span();   /* the previous span's books first */
    eager_cap_ab_step(now);
    eager_cap_step(now);
    /* Precondition check: under eager the host loop keeps the render grid's
     * next slot within one period of now, so this trims nothing. A slot that
     * led by more would leave this tick's span one slot, or none past the
     * clamp to it; its earliest instant at or after now, on the same phase,
     * is the span's first, and s_eager.trimmed counts the breach. */
    const op_time_t first_in = first;
    if (first - now > period) {
        first -= ((first - now) / period) * period;
        s_eager.trimmed++;
    }
    eager_lag_update();
    /* Under FIFO the display shows at most one image per refresh, and the
     * window may sit on a slower monitor than --render-fps was chosen for
     * (480 on a 60 Hz panel): a scene per grid slot would be produced 8x
     * faster than any can be shown and superseded unpresented. Plan on the
     * coarser of the two grids, phase-preserving. */
    eager_lattice_t lat = { 0, 0, 0.0 };
    op_time_t grid_period;            /* the display grid before the cost floor */
    int free_grid;                    /* no display grid: the budget sizes the span */
    lat.on = openpete_native_display_vsync() &&
             openpete_present_thread_lattice(&lat.base, &lat.period);
    period = eager_grid_period(period, &lat, &grid_period, &free_grid);
    eager_span_t sp;
    eager_span_slots(now, first, span_end, period, &lat, &sp);

    /* Budget against what this thread has left of the span once the update
     * VBL has run, and against `prod_end` (the pacer grid instant the produce
     * deadlines are derived from), never the span endpoint. The span is
     * anchored on the capture stamp, which PLL-locks to average tick
     * completion, so it lands about one tick execution past the moment
     * production actually stops; the lag extension above adds more.
     * Budgeting against either prices time this thread never has, so
     * `afford` would exceed the slot count even at a level that cannot keep
     * up, the proactive step would never fire, and the level would fall only
     * after a span was cut short, which is the oscillation the divisor ladder
     * exists to prevent. */
    op_time_t budget;
    const int afford = eager_cap_afford(eager_afford(now, prod_end, sp.ns, &budget), &sp);
    period = eager_span_count(free_grid, afford, grid_period, &sp, period);
    const int ns = sp.ns;
    s_eager.span_free = free_grid;
    eager_plan_dbg(now, first_in, span_end, period, &lat, &sp, budget, afford);
    eager_plan_record(now, &sp, period, budget, afford);
    s_eager.span_cut = 0;
    s_eager.span_warm = s_eager.warm_left > 0;
    s_eager.span_late = s_eager.tick_late > 0;
    s_eager.span_afford = afford;
    s_eager.span_made = 0;
    s_eager.span_pub = 0;
    s_eager.spare = 0;
    s_eager.stop = OP_EAGER_CUT_NONE;

    s_eager.n = 0; s_eager.i = 0;
    if (s_eager.policy == 1) {
        int k = afford < ns ? afford : ns;
        const op_time_t len = sp.end - sp.first;
        for (int q = 0; q < k; q++) s_eager.target[s_eager.n++] = sp.first + (len * q) / k;
    } else if (s_eager.span_fill) {
        for (int q = 0; q < s_eager.fill_k; q++)
            s_eager.target[s_eager.n++] = sp.slots[op_eager_spread_slot(q, s_eager.fill_k, ns)];
    } else {
        for (int q = 0; q < ns; q += s_eager.level) s_eager.target[s_eager.n++] = sp.slots[q];
    }
    s_eager.last_target = s_eager.n ? s_eager.target[s_eager.n - 1] : 0;
    s_eager.span_seed = s_eager.n;
    s_eager.span_replanned = 0;
    eager_shed_unplanned(sp.first, sp.end, grid_period, period, ns);
    eager_prof_plan(budget, afford, ns, period, grid_period, free_grid, lat.on);
    s_eager.spans++;
}

/** @brief Fold one scene's production time into the per-scene cost mean and deviation that eager_tail_cost and eager_afford read.
 *
 * Inside the re-warm window (eager_note_live) a scene cheaper than the mean
 * replaces it and halves the deviation. Otherwise one stall enters the mean
 * and the deviation at a bounded multiple of the mean
 * (op_eager_cost_sample), the deviation's step at most the mean plus four
 * deviations, over a 32-sample window, and the mean over an 8-sample window.
 * @param span The scene's production time in nanoseconds. */
static void eager_cost_note(op_time_t span)
{
    /* Inside the re-warm window a scene cheaper than the mean replaces it and
     * halves the deviation: the first scenes of a level are slow, and at one
     * scene per span an eighth per scene takes dozens of scenes to forget them. */
    if (s_eager.warm_left > 0) {
        s_eager.warm_left--;
        if (s_eager.cost_n && span < s_eager.cost_ema) {
            s_eager.cost_ema = span;
            s_eager.cost_dev /= 2;
            if (s_eager.cost_n < EAGER_COST_WARMUP) s_eager.cost_n++;
            return;
        }
    }
    /* One stall enters the mean and the deviation at a bounded multiple of the mean. */
    const op_time_t sample = s_eager.cost_n ? op_eager_cost_sample(s_eager.cost_ema, span) : span;
    if (s_eager.cost_n) {
        op_time_t d = sample > s_eager.cost_ema ? sample - s_eager.cost_ema : s_eager.cost_ema - sample;
        /* A sample past the bound is one stall, not the width of the cost
         * distribution. Left whole, 8 * cost_dev outgrows the span's budget and
         * decimates the rate until the slow window below forgets it, about two
         * seconds after a single 140 ms scene. The bound moves with the mean, so
         * a real rise in cost is still learned. */
        const op_time_t d_cap = s_eager.cost_ema + 4 * s_eager.cost_dev;
        if (d > d_cap) d = d_cap;
        /* Slower than the mean: the deviation feeds the plan's capacity
         * verdict, and an 8-sample window is half a tick - short enough that a
         * single costly scene moves the level and starts a limit cycle. */
        s_eager.cost_dev = (s_eager.cost_dev * 31 + d) / 32;
    }
    s_eager.cost_ema = s_eager.cost_n ? (s_eager.cost_ema * 7 + sample) / 8 : span;
    if (s_eager.cost_n < EAGER_COST_WARMUP) s_eager.cost_n++;
}

/** @brief Extract one scene for display instant t and publish it into the scene ring.
 *
 * Syncs the VRAM mirror (the guest is idle), sets the interpolation present
 * target to the animation instant t - lag, runs the re-extract, flushes the
 * mods' present work and publishes the scene tagged with t, its kind and
 * alpha; a published scene kicks the present thread, a refused one counts
 * as refused. The canonical scene, first of the span, also raises
 * g_openpete_canon_lag_leg for the re-extract. An intermediate scene first
 * spends the OPENPETE_PACE_INJECT_PRESENT_US fault injection (none unless
 * set) inside the measured span. The scene's production time, bounded by
 * op_eager_cost_sample, updates the cost mean and deviation that
 * eager_tail_cost reads; unbounded, it is logged as its PRESENT_SPAN.
 * @param t Display instant on the op_now() clock.
 * @param canonical Nonzero for the span's canonical scene. */
static void eager_produce_one(op_time_t t, int canonical) OP_REQUIRES(sim)
{
    extern int g_openpete_canon_lag_leg;
    openpete_native_reextract_fn re = openpete_native_get_reextract();
    const op_time_t t0 = op_now();
    const int kind = canonical ? OP_PACE_CANONICAL : OP_PACE_INTERMEDIATE;
    op_time_t rex_ns;
    /* The scene's production is the profiler's root region "scene" (flagged
     * intermediate for an in-between scene), closed before the SCENE marker
     * so its records bind to this scene; its parts are named regions. */
    {
      OP_PROF_FLAGS_SCOPE("scene", canonical ? 0u : OP_PROF_F_INTERMEDIATE);
      if (!canonical) openpete_pace_inject_present();   /* fault injection, off by default */
      uint64_t pt0 = openpete_phase_timers_on() ? openpete_phase_now_us() : 0;
      { OP_PROF_SCOPE("scene.vram_sync");
        openpete_native_vram_sync(); }             /* guest idle: the mirror cannot tear */
      openpete_interp_set_present_target(t - s_eager.lag);   /* the animation instant this display instant shows */
      if (canonical) g_openpete_canon_lag_leg = 1;
      { OP_PROF_SCOPE("scene.open");
        openpete_native_scene_eager_begin();
        openpete_bugrec_subtick_begin(kind, BUGREC_SUB_EAGER); }
      const op_time_t rex0 = op_now();
      { OP_PROF_SCOPE("scene.rex");
        re(&g_game.cpu); }
      rex_ns = op_now() - rex0;
      { OP_PROF_SCOPE("scene.sig_report");
        openpete_native_scene_sig_report(canonical); }
      g_openpete_canon_lag_leg = 0;
      { OP_PROF_SCOPE("scene.mods");
        openpete_mods_present_flush(&g_game.cpu, canonical); }
      openpete_interp_set_present_target(0);
      OP_PROF_SCOPE("scene.publish");
      int paired = 0;
      const double alpha = openpete_pace_extract_alpha(&paired);
      const int slot = openpete_native_scene_publish(t, kind, alpha, paired);
      uint32_t scene_id = 0, stamp = 0;
      openpete_native_scene_slot_ids(slot, &scene_id, &stamp);
      openpete_bugrec_subtick_end(alpha, paired, slot, scene_id, stamp, (int64_t)t);
      if (slot >= 0) {
          openpete_present_thread_kick();
          s_eager.produced++;
          s_eager.span_pub++;
          if (!canonical) s_eager.produced_inter++;
      }
      else s_eager.refused++;
      if (pt0) openpete_phase_add(OPPT_REX_TOTAL, openpete_phase_now_us() - pt0);
    }
    if (op_prof_on())
        op_prof_scene((uint32_t)openpete_canon_tick,
                      canonical ? OP_PROF_CANONICAL : OP_PROF_INTERMEDIATE);
    const op_time_t span = op_now() - t0;
    openpete_pace_present_span((uint8_t)kind, t0, t0 + span, rex_ns);
    eager_cost_note(span);
}

/** @brief Record how many more scenes the produce guard would have started when the span's plan finished.
 *
 * Before the update VBL (a deadline short of produce_end) the span's second
 * window is still to come, so the room is at least
 * OP_EAGER_LADDER_MAX_AFFORD. After it, the guard starts a scene while its
 * start plus the tail cost is at or before the deadline, so the room is 1
 * plus the mean costs that fit in the rest, or 0. Read by the free span's
 * count controller at the next plan.
 * @param now The instant the plan's last scene finished.
 * @param deadline The deadline the produce call ran under.
 * @param tail The per-scene tail cost the guard used. */
static void eager_note_spare(op_time_t now, op_time_t deadline, op_time_t tail)
{
    if (deadline < s_eager.produce_end) { s_eager.spare = OP_EAGER_LADDER_MAX_AFFORD; return; }
    s_eager.spare = op_eager_guard_room(deadline - now, s_eager.cost_ema, tail);
}

/** @brief Re-plan the open free span's unproduced instants from the room left before deadline, after the update VBL.
 *
 * Runs between scenes. On a free span planned by the count controller
 * (no display grid, divisor policy, level not pinned, OPENPETE_EAGER_REPLAN
 * not 0), past the cost warmup, under a deadline at or after produce_end
 * (the window after the update VBL, which ends the span's production), with
 * at least one instant produced and one left: the scenes left are the
 * produce guard's count for the room (op_eager_guard_room: the tail cost for
 * the last, the mean for each other), and the unproduced instants are
 * re-spaced evenly between the last produced instant and the span's last
 * instant, which stays (op_eager_replan), at no less than eager_free_floor.
 * The produced instants and last_target are unchanged, so the next span
 * continues after the same instant. A display-grid span is never re-planned.
 * @param now The scene boundary instant.
 * @param deadline The deadline the produce call runs under.
 * @param tail The per-scene tail cost the guard uses. */
static void eager_replan(op_time_t now, op_time_t deadline, op_time_t tail)
{
    if (!s_eager.span_free || !s_eager.replan_on || s_eager.policy != 0 || s_eager.pinned) return;
    if (s_eager.cost_n < EAGER_COST_WARMUP || deadline < s_eager.produce_end) return;
    if (s_eager.i < 1 || s_eager.i >= s_eager.n) return;
    const int m = op_eager_guard_room(deadline - now, s_eager.cost_ema, tail);
    const int n = op_eager_replan(s_eager.target, s_eager.i, s_eager.n, m, eager_free_floor(s_eager.span_grid, 1),
                                  EAGER_MAX_TARGETS);
    if (n != s_eager.n && !s_eager.span_replanned) { s_eager.span_replanned = 1; s_eager.replanned++; }
    s_eager.n = n;
}

/** @brief Produce the span's planned scenes in order while they fit before deadline.
 *
 * The canonical scene, first of the span, is always produced. A later scene
 * is skipped, and recorded as a PACE_SHED_INSTANT_PASSED shed, when its
 * instant is more than half a period past; the walk
 * stops when the scene ring has one free slot or less, or when the tail
 * cost of one more scene would pass the deadline, records which in
 * s_eager.stop, and resumes from the same scene on the next call. The checks
 * run between scenes only, after eager_replan has re-spaced a free span's
 * unproduced instants to the room left. Each produced scene counts in
 * span_made. When the plan's last scene finishes, eager_note_spare records
 * the room left.
 * @param deadline Time by which production must stop.
 * @param period Render-grid period in nanoseconds.
 * @return The number of scenes produced. */
static int eager_produce(op_time_t deadline, op_time_t period) OP_REQUIRES(sim)
{
    int made = 0;
    const op_time_t tail = eager_tail_cost();
    while (s_eager.i < s_eager.n) {
        const op_time_t now = op_now();
        eager_replan(now, deadline, tail);
        const op_time_t t = s_eager.target[s_eager.i];
        const int canonical = s_eager.i == 0;
        if (!canonical && t < now - period / 2) {   /* its instant is gone */
            eager_shed(PACE_SHED_INSTANT_PASSED, t - now);
            s_eager.i++; s_eager.skipped_late++; continue;
        }
        if (!canonical && openpete_native_scene_ring_free() <= 1) {   /* ring full: resume next iteration */
            s_eager.stop = OP_EAGER_CUT_RING;
            break;
        }
        if (!canonical && now + tail > deadline) {
            s_eager.stop = OP_EAGER_CUT_DEADLINE;
            static int dbg = -1;
            if (dbg < 0) { const char *e = getenv("OPENPETE_EAGER_DBG"); dbg = (e && *e && *e != '0'); }
            if (dbg) fprintf(stderr, "[eager f=%u] stop: %d/%d produced, deadline in %.2fms, tail %.3fms\n",
                             openpete_frame, s_eager.i, s_eager.n, (double)(deadline - now) / 1e6, (double)tail / 1e6);
            break;
        }
        eager_produce_one(t, canonical);
        s_eager.i++; made++; s_eager.span_made++;
        if (s_eager.i == s_eager.n) eager_note_spare(op_now(), deadline, tail);
    }
    return made;
}

/* The re-warm window's length, in scenes on this thread and in renders on the present thread. */
#define EAGER_REWARM_SCENES 32

/** @brief Record whether the world is live, opening the cost averages' re-warm window when it turns live.
 *
 * At a not-live to live edge (boot, a load that stops canonical presents,
 * a discontinuity's hold) and with OPENPETE_EAGER_REWARM on, the next
 * EAGER_REWARM_SCENES scenes may lower the scene cost average to their own
 * cost (eager_produce_one), and the present thread's render and present
 * averages likewise (openpete_present_thread_rewarm). A level edge the
 * terrain walk counted since the previous call (openpete_native_level_edges)
 * opens the window too under OPENPETE_EAGER_REWARM_EDGE: a portal entry
 * keeps canonical presents flowing through its load, so the world never
 * turns not-live there.
 * @param world_live Nonzero when canonical presents are flowing. */
static void eager_note_live(int world_live) OP_REQUIRES(sim)
{
    const unsigned edges = openpete_native_level_edges();
    const int edge = edges != s_eager.edges_seen && s_eager.rewarm_edge_on;
    s_eager.edges_seen = edges;
    if (world_live && (!s_eager.live_prev || edge) && s_eager.rewarm_on) {
        if (edge && s_eager.live_prev) s_eager.edge_windows++;
        s_eager.warm_left = EAGER_REWARM_SCENES;
        openpete_present_thread_rewarm(EAGER_REWARM_SCENES);
    }
    s_eager.live_prev = world_live;
    s_eager.world_live = world_live;
}

/** @brief Measure the VBLs since the previous canonical present.
 *
 * A guest discontinuity zeroes canon_frame, so the first canonical after one
 * measures an unknown gap of 0.
 * @return The gap in VBLs, 0 when unknown. */
static unsigned eager_canon_gap(void)
{
    const unsigned f = (unsigned)openpete_frame;
    const unsigned gap = s_eager.canon_frame && f > s_eager.canon_frame ? f - s_eager.canon_frame : 0;
    s_eager.canon_frame = f;
    return gap;
}

/* Documented at the declaration in eager.h. */
void eager_canonical(op_time_t now, op_time_t slot, op_time_t period,
                     op_time_t prod_end, op_time_t produce_end, int world_live) OP_REQUIRES(sim)
{
    const op_time_t curr = openpete_interp_curr_tick_ns();
    op_time_t first = slot;
    while (first < now) first += period;    /* the next slot not yet gone */
    /* The span is anchored on the newest capture's stamp - unless
     * that stamp is stale. Captures dedupe when the interpolated
     * regions do not change (title screen, a cutscene holding
     * the camera), and a span anchored on a stamp a tick or
     * more old collapses to one or two slots while the inline
     * path keeps presenting the live scene at full rate. Anchor
     * on this tick's landing instead. */
    const op_time_t tp = (op_time_t)g_openpete_tick_period_ns;
    op_time_t anchor = (curr && now - curr < 2 * tp) ? curr : now;
    op_time_t span_end = anchor + tp;
    /* A canonical every VBL (the pause window's grow and shrink): the next
     * canonical lands one VBL on, not one tick, and the interp ring's stamp
     * lags it, since the ring commits at most every second VBL on a state the
     * 2D does not live in. A span of one tick from that stamp alternates a
     * scene run past the next canonical, drawn from the 2D carry that canonical
     * replaces, with a span of one slot. The span is this VBL's stamp plus one
     * VBL, over which the 2D carry's alpha runs 0 to 1. Its budget is one VBL
     * less the tick's mean cost, since there is no update VBL to produce
     * through; it is priced from now rather than from the next deadline, so a
     * tick that started late, which the produce guard absorbs, is not read as
     * the capacity of every span of the run. */
    if (eager_canon_gap() == 1) {
        const op_time_t tick = openpete_interp_tick_stamp();
        const op_time_t vbl_anchor = (tick && now - tick < tp) ? tick : now;
        anchor = vbl_anchor;
        span_end = vbl_anchor + tp / 2;
        prod_end = now + tp / 2 - s_eager.tick_ema;
    }
    s_eager.anchor_age = now - anchor;
    eager_plan(now, first, period, span_end, prod_end);
    s_eager.produce_end = produce_end;
    /* The canonical scene, then as much of the span as fits
     * before the update VBL is due - the latter only while
     * the world is live. */
    eager_note_live(world_live);
    eager_produce(world_live ? produce_end - 500000 : now, period);
}

/* Documented at the declaration in eager.h. */
int eager_idle(int world_live, int may_produce, op_time_t deadline, op_time_t period) OP_REQUIRES(sim)
{
    /* While canonical presents are not flowing (CD load, level transition,
     * a long producer stall) the guest is rewriting RAM and a re-extract
     * would read it mid-overwrite: the display holds the last image and no
     * scene is produced. */
    eager_note_live(world_live);
    if (!(may_produce && world_live)) return 0;
    eager_produce(deadline, period);
    return 1;
}

/** @brief Log the eager-production totals at shutdown.
 *
 * One `eager` info line: spans, scenes produced, skipped because their
 * instant passed, refused because the ring was full, and cut short (of
 * those, cut at the produce guard's deadline and cut by a full ring); the
 * divisor level and its moves (shown as n/a under the greedy policy, where
 * the level does not decide the plan); scene cost mean and deviation; the
 * lag and the tick execution-time mean and deviation; the policy; the spans
 * planned above the standing level for an isolated shortfall; the spans
 * whose render-grid slot was pulled back to within a period of now; the
 * cadence policy at exit and the spans the fill cadence planned; the
 * re-warm windows a level edge opened; the free spans whose planned count
 * the re-plan changed; then the presenter cap's line (eager_cap_report).
 * Silent when no span ran. */
static void eager_report(void)
{
    if (!s_eager.spans) return;
    /* Under the greedy policy the divisor is bookkeeping only (the plan
     * comes from `afford`), so it is reported as such. */
    OPL_INFO_F("eager", "%u span(s): %u scene(s) produced, %u skipped (instant passed), "
               "%u refused (ring full), %u cut short (%u deadline, %u ring); level now %s%d (%u down, %u up); "
               "scene cost %.3f ms ema, %.3f ms dev; lag %.3f ms (tick %.3f ms ema, %.3f dev); policy %s; "
               "%u span(s) above the level; %u first slot(s) pulled back; cadence %s, %u fill span(s); "
               "%u re-warm window(s) opened by a level edge; %u span(s) re-planned",
               s_eager.spans, s_eager.produced, s_eager.skipped_late, s_eager.refused,
               s_eager.cut, s_eager.cut_deadline, s_eager.cut_ring, s_eager.policy ? "n/a " : "", s_eager.ladder.level, s_eager.level_down, s_eager.level_up,
               (double)s_eager.cost_ema / 1e6, (double)s_eager.cost_dev / 1e6,
               (double)s_eager.lag / 1e6, (double)s_eager.tick_ema / 1e6, (double)s_eager.tick_dev / 1e6,
               s_eager.policy ? "greedy" : "divisor", s_eager.span_held, s_eager.trimmed,
               openpete_get_cadence() == OPENPETE_CADENCE_FILL ? "fill" : "even", s_eager.fill_spans,
               s_eager.edge_windows, s_eager.replanned);
    eager_cap_report();
}

/** @brief Drop the eager scene plan on a guest discontinuity (a savestate load from a file, or a non-quiet in-memory state load).
 *
 * Clears the planned targets, the seed count, the last target and the canonical gap's origin,
 * and resets the decaying tick-time peak to the tick mean. The scene ring flushes its own pending
 * scenes. Registered with openpete_on_guest_discontinuity. A rewind step (a
 * quiet in-memory load) and a level load do not call it: a level load stops
 * canonical presents, so production is withheld (world_live) until they
 * resume, the first span after it does not continue a last target more than
 * a period in the past, and the world going live opens the re-warm window
 * (eager_note_live). */
static void eager_on_discontinuity(void) { s_eager.n = s_eager.i = s_eager.span_seed = 0; s_eager.last_target = 0; s_eager.tick_peak = s_eager.tick_ema; s_eager.canon_frame = 0; }

/* The run's frame-time log, for presents made on the present thread; NULL
 * until eager production starts. */
static openpete_frame_time_log_t* s_eager_ftl;
/** @brief Record a present made on the present thread in the profiler trace and the frame-time log.
 *
 * Runs on the present thread after each present that reached the display.
 * The profiler's PRESENT record carries the kind and openpete_canon_tick at
 * the call. The frame-time log record carries openpete_frame at the call.
 * @param ctx Unused.
 * @param kind OP_PACE_CANONICAL, OP_PACE_INTERMEDIATE or OP_PACE_REPEAT.
 * @param alpha Unused.
 * @param paired Unused. */
static void eager_present_cb(void *ctx, int kind, double alpha, int paired)
{
    OP_ROLE_ASSERT(present);
    (void)ctx; (void)alpha; (void)paired;
    present_record_shown(s_eager_ftl, kind);
}

/* Documented at the declaration in eager.h. */
void host_eager_start(openpete_frame_time_log_t* ftl, const openpete_options_t* opts) OP_REQUIRES(sim)
{
    openpete_set_cadence(opts->cadence);
    if (!(openpete_native_scene_enabled() && !opts->headless && openpete_present_thread_start()))
        return;
    eager_init();
    s_eager_ftl = ftl;
    openpete_present_thread_set_callback(eager_present_cb, NULL);
    openpete_on_guest_discontinuity(eager_on_discontinuity);
    openpete_on_guest_discontinuity(openpete_native_scene_ring_flush);
}

/* Documented at the declaration in eager.h. */
void eager_exit_report(unsigned inline_rex)
{
    eager_report();
    if (s_eager.produced_inter || inline_rex)
        OPL_INFO_F("sub-tick", "%u re-extract(s) presented: %u eager, %u inline",
                   s_eager.produced_inter + inline_rex,
                   s_eager.produced_inter, inline_rex);
}

#endif /* OPENPETE_HAS_PSYCROSS */
