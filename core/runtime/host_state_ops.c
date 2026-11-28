// core/runtime/host_state_ops.c - the savestate request queue drained at
// final yields, the active quick-save slot, the --state-stress harnesses and
// the rewind driver.

#include "host_state_ops.h"
#include "host_control.h"
#include "run_internal.h"
#include "host_input_tick.h"
#include "runahead.h"

#include "openpete.h"
#include "openpete_fiber.h"
#include "openpete_handoff.h"
#include "openpete_bugrec.h"
#include "openpete_bugreport.h"
#include "openpete_bugreport_replay.h"
#include "openpete_log.h"
#include "openpete_memcard.h"
#include "openpete_mods.h"
#include "openpete_state.h"
#include "game_extras.h"
#include "../rhi/rhi_gpu_audit.h"
#include "openpete_keymap.h"
#include "openpete_osd.h"
#include "openpete_rewind.h"
#include "openpete_input_log.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_pace_log.h"
#include "PsyX/PsyX_public.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

extern void PsyX_SPUAL_Speculate(int on, int commit);

#ifdef OPENPETE_HAS_PSYCROSS
/* Snapshots (savestate, rewind keyframe, runahead) and loads happen only
 * while the game fiber is parked at a psyq_VSync wait's final yield
 * (openpete_yield_final). Below any other yield the fiber's host call stack
 * is mid-delivery (an intermediate VBL of VSync(n > 1), a polling-loop or
 * spin-tick VBL, a schedule charge, a flushed VBLANK, a stub-miss yield);
 * the stack is not snapshotted, so a state taken there or loaded there
 * resumes the wrong continuation.
 *
 * A load also needs the right final yield: each records its park site (the
 * psyq_VSync call's guest ra and sp, openpete_yield_site), every snapshot
 * stores the site it was taken at, and a load applies only while the fiber
 * is parked at a final yield with that site. At another site the loaded
 * registers would run the parked call's continuation (its caller's code
 * with the saved call's s-registers). A load that finds another site waits:
 * a queued file or slot load up to STATE_LOAD_SITE_WAIT final yields and
 * is refused after them; the rewind seek takes the newest keyframe saved
 * at the current site; the stress rollback waits up to its bound and then
 * re-arms; the runahead rollback continues the speculation to the
 * snapshot's site, through a transition when the speculation leaves
 * gameplay, and adopts it at the bound.
 *
 * One-shot requests (the save and load hotkeys, a mod's request, the
 * scheduled --state-save-on-frame / --state-load-on-frame) enter a queue
 * whenever they are raised; state_ops_drain runs the queue in arrival order
 * at a final yield and keeps it otherwise. The periodic state work (the
 * stress harnesses, the rewind keyframe cadence and seek, the process
 * handoff, the runahead snapshot) runs only on an iteration whose drain
 * reported a final yield, and sits out the others. */
typedef enum {
    STATE_OP_SAVE_SLOT,   /* openpete_state_save to a slot */
    STATE_OP_LOAD_SLOT,   /* openpete_state_load from a slot */
    STATE_OP_SAVE_FILE,   /* openpete_state_save_file to a path */
    STATE_OP_LOAD_FILE,   /* openpete_state_load_file from a path */
    STATE_OP_BUG_REPORT,  /* openpete_bugreport_press: the bug-report bundle's press */
} state_op_kind;

typedef struct {
    state_op_kind kind;
    unsigned slot;        /* the slot of a slot op */
    const char* path;     /* the path of a file op */
    int deferred;         /* counted once in s_state_ops_deferred */
    unsigned site_waits;  /* final yields a load spent at another park site */
} state_op_t;

/* Final yields a queued load waits for the fiber to park at its site. A
 * game's tick parks at each of its frame-wait sites every tick, so a site
 * absent this long belongs to a mode the run has left (a pause menu, a
 * transition): loading at another site corrupts the timeline, so the load
 * is refused instead. 1800 final yields is at least 30 s of play. */
#define STATE_LOAD_SITE_WAIT 1800u

unsigned long long s_site_deferred[SITE_DEF_N];
unsigned long long s_site_refused = 0;    /* queued loads refused at STATE_LOAD_SITE_WAIT */
unsigned long long s_site_wait_yields = 0; /* final yields queued loads spent at another site */
unsigned long long s_site_abandoned = 0;  /* stress rollbacks re-armed at their bound */
static unsigned long long s_rewind_steps OP_GUARDED_BY(sim) = 0;      /* rewind steps that loaded a keyframe */
static unsigned long long s_rewind_short OP_GUARDED_BY(sim) = 0;      /* rewind replays that ended before their target */
static unsigned long long s_rewind_unseen_last OP_GUARDED_BY(sim) = 0; /* replays whose last draw was predicted unseen */
static unsigned long long s_rewind_seen_early OP_GUARDED_BY(sim) = 0;  /* replayed draws predicted seen that a later draw followed */
static unsigned long long s_rewind_log_moved OP_GUARDED_BY(sim) = 0;  /* rewinds after which the input log's offset to the frame changed */

/* The run options the bug-report press records (argv, disc). */
static const openpete_options_t* s_bug_opts;

#define STATE_OP_QUEUE_LEN 8
static state_op_t s_state_ops[STATE_OP_QUEUE_LEN];
static int s_state_op_n = 0;
unsigned long long s_state_ops_deferred[2];  /* requests that waited: [0] saves, [1] loads */
unsigned long long s_state_yields_skipped = 0;  /* drains at an intermediate yield */
/* The share of s_state_ops_deferred first deferred at a yield an emitted
 * polling loop's delivery took (openpete_yield_loop_header), and the first
 * such loop's header and feed key. */
unsigned long long s_loop_deferred = 0;
uint32_t s_loop_deferred_header = 0u;
unsigned s_loop_deferred_key = 0u;

void count_loop_deferral(void) {
    if (!openpete_yield_loop_header) return;
    if (s_loop_deferred++ == 0u) {
        s_loop_deferred_header = openpete_yield_loop_header;
        s_loop_deferred_key = openpete_feed_key_now();
    }
}

/* The active quick-save slot, shared by the slot keys and the dev overlay's
 * Savestate row. A set from another thread raises the announce flag; the host
 * loop announces, since the OSD queue is fed from it. */
static SDL_AtomicInt s_state_slot;
static SDL_AtomicInt s_state_slot_announce;
/* Save/load requests from the dev overlay, bits consumed by state_ops_collect. */
enum { STATE_REQ_SAVE = 1, STATE_REQ_LOAD = 2 };
static SDL_AtomicInt s_state_slot_req;
/* 1 when the active slot's file exists: set on each slot change and after a
 * save to that slot, read by the dev overlay's Load button. */
static SDL_AtomicInt s_state_slot_full;

unsigned openpete_state_active_slot(void) { return (unsigned)SDL_GetAtomicInt(&s_state_slot); }

int openpete_state_active_slot_full(void) { return SDL_GetAtomicInt(&s_state_slot_full); }

void openpete_state_set_active_slot(unsigned slot) {
    if (slot >= OPENPETE_STATE_NUM_SLOTS) return;
    SDL_SetAtomicInt(&s_state_slot, (int)slot);
    SDL_SetAtomicInt(&s_state_slot_announce, 1);
}

void openpete_state_request(int load) {
    const int bit = load ? STATE_REQ_LOAD : STATE_REQ_SAVE;
    int cur;
    do cur = SDL_GetAtomicInt(&s_state_slot_req);
    while (!SDL_CompareAndSwapAtomicInt(&s_state_slot_req, cur, cur | bit));
}

/** @brief Queue one savestate request for the next final yield.
 * @param kind The operation.
 * @param slot The slot of a slot operation.
 * @param path The path of a file operation. */
static void state_op_push(state_op_kind kind, unsigned slot, const char* path) {
    if (s_state_op_n == STATE_OP_QUEUE_LEN) {
        OPL_WARN_F("savestate", "state-op queue full (%d): request dropped", STATE_OP_QUEUE_LEN);
        return;
    }
    s_state_ops[s_state_op_n++] = (state_op_t){kind, slot, path, 0, 0u};
}

/* A request's outcome, as state_op_run and state_ops_drain report it: the
 * first three are openpete_state_save / openpete_state_load's return values. */
enum {
    STATE_OP_FAILED = -1,   /* save or load failed or refused; running state untouched */
    STATE_OP_DONE = 0,      /* saved, or loaded */
    STATE_OP_SITE = 1,      /* a load found another park site than its state's */
    STATE_OP_SITE_REFUSED = 2, /* that load waited STATE_LOAD_SITE_WAIT final yields */
};

/** @brief Run one queued savestate request; a load that succeeds re-anchors the host frame counter.
 *
 * The re-anchor makes the one-shot triggers (screenshot-on-frame,
 * ram-dump-on-frame, frame-limit) reason about time from the load as on a
 * fresh run.
 * @param op The request.
 * @return STATE_OP_DONE, STATE_OP_FAILED, or STATE_OP_SITE when a load found
 *         the fiber parked at another site than its state's (nothing
 *         restored). */
static int state_op_run(const state_op_t* op) OP_REQUIRES(sim) {
    unsigned lf = openpete_frame;
    int rc = STATE_OP_FAILED;
    switch (op->kind) {
    case STATE_OP_SAVE_SLOT:
        rc = openpete_state_save(op->slot, &g_game.cpu, openpete_frame);
        if (rc == 0 && op->slot == openpete_state_active_slot())
            SDL_SetAtomicInt(&s_state_slot_full, 1);
        return rc;
    case STATE_OP_SAVE_FILE:
        return openpete_state_save_file(op->path, &g_game.cpu, openpete_frame);
    case STATE_OP_LOAD_SLOT:
        rc = openpete_state_load(op->slot, &g_game.cpu, &lf);
        break;
    case STATE_OP_LOAD_FILE:
        rc = openpete_state_load_file(op->path, &g_game.cpu, &lf);
        break;
    case STATE_OP_BUG_REPORT:
        return openpete_bugreport_press(&g_game.cpu, s_bug_opts) == 0 ? STATE_OP_DONE
                                                                       : STATE_OP_FAILED;
    }
    if (rc == STATE_OP_DONE) openpete_frame_set(lf);
    return rc;
}

/** @brief Keep a load that found another park site, or refuse it once it has waited STATE_LOAD_SITE_WAIT final yields.
 * @param op The load request.
 * @return 1 when the request stays queued, 0 when it was refused. */
static int state_op_site_wait(state_op_t* op) {
    s_site_wait_yields++;
    if (op->site_waits++ == 0u) s_site_deferred[SITE_DEF_LOAD]++;
    if (op->site_waits < STATE_LOAD_SITE_WAIT) return 1;
    s_site_refused++;
    OPL_ERROR_F("savestate", "load of %s refused: the fiber did not park at the state's "
                "site within %u final yields (parked at ra=0x%08X sp=0x%08X); running "
                "state untouched", op->kind == STATE_OP_LOAD_FILE ? op->path : "the slot",
                STATE_LOAD_SITE_WAIT, openpete_yield_site.ra, openpete_yield_site.sp);
    return 0;
}

/* The OSD coalescing key every slot toast shares: a save, a load and a slot
 * switch in quick succession repaint one toast instead of stacking three. */
#define STATE_SLOT_OSD_KEY "state.slot"

/** @brief Post the OSD toast for a finished slot save or slot load.
 *
 * File requests (the scheduled --state-save-on-frame / --state-load-on-frame
 * one-shots) post nothing: no player pressed a key for them. A failed slot
 * load whose slot file does not exist is reported as an empty slot; every
 * other failure (a state from another game, build or modset, an unreadable
 * file, a park site that did not recur) as a refused load, whose reason is
 * in the log.
 * @param op The finished request.
 * @param rc STATE_OP_DONE, STATE_OP_FAILED or STATE_OP_SITE_REFUSED. */
static void state_op_toast(const state_op_t* op, int rc) {
    const char* fmt = NULL;
    if (op->kind == STATE_OP_SAVE_SLOT)
        fmt = rc == STATE_OP_DONE ? "STATE SAVED: SLOT %u" : "SAVE FAILED: SLOT %u";
    else if (op->kind != STATE_OP_LOAD_SLOT)
        return;
    else if (rc == STATE_OP_DONE)
        fmt = "STATE LOADED: SLOT %u";
    else
        fmt = rc == STATE_OP_SITE_REFUSED || openpete_state_slot_present(op->slot)
                  ? "LOAD REFUSED: SLOT %u" : "LOAD: SLOT %u EMPTY";
    openpete_osd_notify("host", STATE_SLOT_OSD_KEY, 0, fmt, op->slot);
}

/** @brief Log and toast the hotkey slot after a slot switch, with whether a state is saved in it.
 * @param slot The new active slot. */
static void state_slot_announce(unsigned slot) {
    const int present = openpete_state_slot_present(slot);
    OPL_INFO_F("savestate", "active slot -> %u%s", slot, present ? " (occupied)" : " (empty)");
    openpete_osd_notify("host", STATE_SLOT_OSD_KEY, 0, "STATE SLOT: %u %s", slot,
                        present ? "OCCUPIED" : "EMPTY");
}

int state_ops_drain(void) OP_REQUIRES(sim) {
    if (!openpete_yield_final) {
        s_state_yields_skipped++;
        for (int i = 0; i < s_state_op_n; i++) {
            state_op_t* op = &s_state_ops[i];
            if (op->deferred) continue;
            op->deferred = 1;
            s_state_ops_deferred[op->kind == STATE_OP_LOAD_SLOT || op->kind == STATE_OP_LOAD_FILE]++;
            count_loop_deferral();
        }
        return 0;
    }
    int done = 0;
    while (done < s_state_op_n) {
        state_op_t* op = &s_state_ops[done];
        int rc = state_op_run(op);
        if (rc == STATE_OP_SITE) {
            if (state_op_site_wait(op)) break;
            rc = STATE_OP_SITE_REFUSED;
        }
        state_op_toast(op, rc);
        done++;
    }
    memmove(s_state_ops, s_state_ops + done, (size_t)(s_state_op_n - done) * sizeof s_state_ops[0]);
    s_state_op_n -= done;
    return 1;
}

void state_ops_collect(const openpete_options_t* opts) OP_REQUIRES(sim) {
    static bool sched_saved = false, sched_loaded = false;
    s_bug_opts = opts;
    if (openpete_bugreport_due(hc_edge(OPK_BUG_REPORT, HC_GATE_MENU)))
        state_op_push(STATE_OP_BUG_REPORT, 0, NULL);
    const unsigned slot = openpete_state_active_slot();
    const int req = SDL_SetAtomicInt(&s_state_slot_req, 0);
    /* The save and load keys default to the number row. The slot is the one
     * the dev overlay shares. */
    if (hc_edge(OPK_STATE_SAVE, HC_GATE_MENU) || (req & STATE_REQ_SAVE))
        state_op_push(STATE_OP_SAVE_SLOT, slot, NULL);
    if (hc_edge(OPK_STATE_LOAD, HC_GATE_MENU) || (req & STATE_REQ_LOAD))
        state_op_push(STATE_OP_LOAD_SLOT, slot, NULL);
    const int key = (int)openpete_feed_key_now();
    if (!sched_saved && opts->state_save_on_frame >= 0 && key >= opts->state_save_on_frame) {
        sched_saved = true;
        state_op_push(STATE_OP_SAVE_FILE, 0, opts->state_save_output);
    }
    if (!sched_loaded && opts->state_load_on_frame >= 0 && key >= opts->state_load_on_frame) {
        sched_loaded = true;
        if (!opts->state_load_input)
            OPL_ERROR_F("savestate", "--state-load-on-frame armed but no --state-load-input");
        else
            state_op_push(STATE_OP_LOAD_FILE, 0, opts->state_load_input);
    }
    if (hc_edge(OPK_STATE_CYCLE, HC_GATE_MENU))
        openpete_state_set_active_slot((slot + 1u) % OPENPETE_STATE_NUM_SLOTS);
    static bool announced_once = false;
    if (SDL_SetAtomicInt(&s_state_slot_announce, 0)) {
        state_slot_announce(openpete_state_active_slot());
    } else if (announced_once) {
        return;
    }
    announced_once = true;
    SDL_SetAtomicInt(&s_state_slot_full, openpete_state_slot_present(openpete_state_active_slot()));
}

static unsigned s_stress_it = 0, s_stress_ops = 0;  /* iterations in the current phase; operations done */

/** @brief --state-stress N: every N iterations, an in-memory save and an immediate quiet restore; a stressed run must be hash-identical to a clean one.
 * @param sm The stress snapshot.
 * @param every N. */
static void state_stress_roundtrip(openpete_state_mem_t* sm, unsigned every) OP_REQUIRES(sim) {
    if (s_stress_it % every != 0) return;
    if (openpete_state_mem_save(sm, &g_game.cpu, openpete_frame) != 0 ||
        openpete_state_mem_load(sm, &g_game.cpu, NULL, 1) != 0)
        return;
    if ((++s_stress_ops & 0x3FFu) == 1u)
        OPL_INFO_F("savestate", "stress: %u roundtrips (f=%u)", s_stress_ops, openpete_frame);
}

/** @brief --state-stress-rewind K: save, run K iterations, restore, replay K (the runahead rollback shape); the replayed window must self-match.
 *
 * Three phases so the timeline still advances: 0 arms (save at F), 1 runs
 * K and rolls back to F, 2 replays K and re-arms at F+K. Armed only in
 * steady-state gameplay: the mem snapshot excludes the boot and load-window
 * sections (VRAM, SPU sample RAM) by contract, so a pending rollback whose
 * window crosses into a transition is abandoned, as runahead is gated off
 * around loads. The rollback applies only at the snapshot's park site: at
 * another site it waits (counted once per rollback), up to 8K + 16
 * iterations past K, the runahead speculation's bound; a site that does not
 * recur by then abandons the rollback and re-arms at the current site.
 * @param sm The stress snapshot.
 * @param k K. */
static void state_stress_rewind(openpete_state_mem_t* sm, unsigned k) OP_REQUIRES(sim) {
    static int armed = 0;
    const int live = game_is_gameplay_live(&g_game.cpu);
    if (armed == 1 && !live) armed = 0;
    if (armed == 0) {
        if (live && openpete_state_mem_save(sm, &g_game.cpu, openpete_frame) == 0) armed = 1;
        s_stress_it = 0;
        return;
    }
    if (s_stress_it < k) return;
    if (armed == 2) { armed = 0; return; }   /* replay done: re-arm here */
    unsigned lf = openpete_frame;
    const int rc = openpete_state_mem_load(sm, &g_game.cpu, &lf, 1);
    if (rc == 1) {
        if (s_stress_it == k) s_site_deferred[SITE_DEF_STRESS]++;
        if (s_stress_it >= 9u * k + 16u) {
            s_site_abandoned++;
            armed = 0;
        }
        return;
    }
    if (rc == 0) {
        openpete_frame_set(lf);
        if ((++s_stress_ops & 0xFFu) == 1u)
            OPL_INFO_F("savestate", "stress-rewind: %u rollbacks (back to f=%u)", s_stress_ops, lf);
    }
    armed = 2;
    s_stress_it = 0;
}

void state_stress_step(const openpete_options_t* opts) OP_REQUIRES(sim) {
    static openpete_state_mem_t* sm = NULL;
    if (opts->state_stress <= 0 && opts->state_stress_rewind <= 0) return;
    if (!sm) sm = openpete_state_mem_new();
    if (!sm) return;
    s_stress_it++;
    if (opts->state_stress > 0) state_stress_roundtrip(sm, (unsigned)opts->state_stress);
    if (opts->state_stress_rewind > 0) state_stress_rewind(sm, (unsigned)opts->state_stress_rewind);
}

/** @brief Stage the input log's record for the tick being re-executed, every slot as recorded, with player 1 as the pad override.
 *
 * A re-executed tick reads every slot exactly as the log recorded it, or
 * the burst replays player 1 faithfully and the other players from stale
 * bytes. Synthetic pads are checked against the log, their only carrier.
 * @param r The record, or NULL when the log has none (nothing staged). */
static void rewind_stage_logged_input(const openpete_input_rec_t* r) {
    if (!r) return;
    input_log_stage_reset(openpete_frame);
    for (int sl = 0; sl < OPENPETE_PAD_SLOTS; sl++) {
        const openpete_input_port_t* pt = &r->port[sl];
        if (pt->present) input_log_stage_port(sl, pt->bits, pt->axes);
    }
    if (synth_pads_enabled()) {
        s_synth_rewind_checks++;
        if (!synth_pads_match(input_log_staged(), openpete_frame)) s_synth_rewind_mismatches++;
    }
    const openpete_input_port_t* p1 = &r->port[0];
    g_PsyX_Pad_InputOverride = p1->bits;
    g_PsyX_Pad_AnalogOverride[0] = p1->axes[2];
    g_PsyX_Pad_AnalogOverride[1] = p1->axes[3];
    g_PsyX_Pad_AnalogOverride[2] = p1->axes[0];
    g_PsyX_Pad_AnalogOverride[3] = p1->axes[1];
    g_PsyX_Pad_OverrideActive = 1;
}

/** @brief Report whether the harness fault OPENPETE_REWIND_FAULT_SHORT=N ends this rewind's replay after its first replayed tick, as a yield that makes no tick ends it: the N-th rewind step of the run (1-based); read once.
 *
 * @param step The 1-based count of rewind steps that loaded a keyframe.
 * @return true when the fault applies to @p step. */
static bool rewind_fault_short(unsigned long long step) {
    static long long s_at = -1;
    if (s_at < 0) {
        const char* e = getenv("OPENPETE_REWIND_FAULT_SHORT");
        s_at = e && *e ? atoll(e) : 0;
    }
    return s_at > 0 && step == (unsigned long long)s_at;
}

/* Documented at the declaration in host_state_ops.h. */
void draw_run_aim(draw_run_t* r, unsigned target) {
    r->target = target;
    r->draw_at = DRAW_RUN_NONE;
    r->period = 2u;
}

/* Documented at the declaration in host_state_ops.h. */
int draw_run_frame_unseen(draw_run_t* r, unsigned frame) {
    if (r->draw_at != DRAW_RUN_NONE && frame > r->draw_at) r->period = frame - r->draw_at;
    r->draw_at = frame;
    return frame + r->period < r->target;
}

/* Documented at the declaration in host_state_ops.h. */
int draw_run_note(draw_run_t* r, int unseen) {
    r->last_unseen = unseen;
    r->drawn = 1;
    r->seen += !unseen;
    return unseen;
}

/* Documented at the declaration in host_state_ops.h. */
int draw_run_close(draw_run_t* r, unsigned long long* unseen_last,
                   unsigned long long* seen_early) {
    const int miss = r->drawn && r->last_unseen;
    if (r->drawn) {
        *unseen_last += (unsigned long long)miss;
        *seen_early += r->seen - (unsigned)!r->last_unseen;
    }
    r->last_unseen = 0;
    r->drawn = 0;
    r->seen = 0u;
    return miss;
}

/* The replay in progress, for openpete_rewind_draw_unseen. */
static draw_run_t s_replay OP_GUARDED_BY(sim) = { 0u, DRAW_RUN_NONE, 2u, 0, 0, 0u };   /* thread-roles: the rewind driver and its replayed draws run on sim */

/* Documented at the declaration in openpete_rewind.h. */
int openpete_rewind_draw_unseen(void) OP_REQUIRES(sim) {
    if (!openpete_replaying) return 0;
    return draw_run_note(&s_replay, draw_run_frame_unseen(&s_replay, openpete_frame));
}

/** @brief Replay logged ticks from a just-loaded keyframe until the frame reaches the target, then count the replay's mispredicted draws.
 *
 * The replay's canonical world draws read openpete_rewind_draw_unseen. A
 * replay whose last draw was predicted unseen (its presented scene was
 * built as unseen) and the draws predicted seen that a later draw followed
 * are counted for the exit summary.
 * @param kf The keyframe the timeline was restored from.
 * @param target The frame the replay runs to.
 * @param fault_short End after the first replayed yield (harness fault). */
static void rewind_replay_ticks(const openpete_rewind_kf_t* kf, unsigned target,
                                bool fault_short) OP_REQUIRES(sim) {
    draw_run_aim(&s_replay, target);
    openpete_replaying = 1;
    PsyX_SPUAL_Speculate(1, 0);
    while (openpete_frame < target) {
        rewind_stage_logged_input(input_log_get(kf->log_idx + (int)(openpete_frame - kf->frame)));
        const unsigned before = openpete_frame;
        game_on_frame_pre(&g_game.cpu, openpete_frame);
        openpete_pace_vbl_suspend(1);
        g_openpete_guest_running = 1;
        openpete_fiber_switch(openpete_game_fiber);
        g_openpete_guest_running = 0;
        openpete_pace_vbl_suspend(0);
        if (openpete_frame == before) break;   /* the fiber made no tick */
        if (fault_short) break;
    }
    openpete_replaying = 0;
    PsyX_SPUAL_Speculate(0, 0);
    (void)draw_run_close(&s_replay, &s_rewind_unseen_last, &s_rewind_seen_early);
}

/** @brief Step the timeline back one tick: quiet-load the covering keyframe, replay forward to the target with the logged inputs, truncate the future and the bug-report recorder's window past it.
 *
 * Interp stores are reset (cheap memsets) so presents snap backward; the
 * full discontinuity broadcast (texture-pack rescan) is not fired per held
 * iteration. Replayed ticks run the main loop's pre-yield hook (per-tick
 * guest mutations are part of the timeline) with presentation
 * announcements and pad-read pacing suspended.
 * The keyframe is the newest at or before the target saved at the park
 * site the fiber is parked at: only that keyframe can be loaded here, and
 * the logged-input replay reaches the target from it whatever its age. With
 * no such keyframe in the window the step is deferred (counted) and the
 * tick runs forward, so a held rewind resumes at the next final yield whose
 * site a keyframe carries.
 * A replay can end before the target (a yield that makes no tick): the
 * input log, the keyframes and the recorder's window are cut at the frame
 * the timeline reached, so the log keeps one entry per VBL up to it and
 * the next tick's entry lands at its own index.
 * @param bk2_active A movie is loaded for replay (it keeps the pad override).
 * @return true when the timeline moved. */
static bool rewind_seek_one_tick(bool bk2_active) OP_REQUIRES(sim) {
    const unsigned target = openpete_frame > 2u ? openpete_frame - 2u : 0u;
    const openpete_rewind_kf_t* kf = openpete_rewind_pick(target, &openpete_yield_site);
    if (!kf) {
        if (openpete_rewind_pick(target, NULL)) {
            s_site_deferred[SITE_DEF_REWIND]++;
            OPL_DEBUG_F("rewind", "no keyframe at or before f=%u was saved at the park site "
                        "ra=0x%08X sp=0x%08X; step deferred", target,
                        openpete_yield_site.ra, openpete_yield_site.sp);
            return false;
        }
        OPL_WARN_F("rewind", "no keyframe covers f=%u (window exhausted or none captured)", target);
        return false;
    }
    const int log_offset = input_log_count() - (int)openpete_frame;
    unsigned lf = openpete_frame;
    if (openpete_rewind_restore(kf, &g_game.cpu, &lf) != 0) return false;
    openpete_frame_set(lf);
    const bool fault_short = rewind_fault_short(++s_rewind_steps);
    openpete_interp_reset();
    openpete_interp_objs_reset();
    rewind_replay_ticks(kf, target, fault_short);
    if (openpete_frame < target) {
        s_rewind_short++;
        OPL_DEBUG_F("rewind", "replay ended at f=%u, short of the target f=%u", openpete_frame, target);
    }
    input_log_lock();
    input_log_truncate(kf->log_idx + (int)(openpete_frame - kf->frame));
    if (input_log_count() - (int)openpete_frame != log_offset && s_rewind_log_moved++ == 0)
        OPL_WARN_F("rewind", "input log holds %d entries at f=%u; its offset to the frame moved from %d",
                   input_log_count(), openpete_frame, log_offset);
    input_log_unlock();
    openpete_rewind_drop_after(openpete_frame);
    openpete_bugrec_rewound(openpete_frame);
    if (!bk2_active) g_PsyX_Pad_OverrideActive = 0;
    return true;
}

/** @brief Report whether every enabled code mod declares its host-state posture (mod.toml state=), warning once when one does not.
 *
 * An undeclared mod's host state desyncs silently when the world moves
 * under it, so a timeline jump is refused while one is enabled.
 * @return true when a timeline jump is allowed. */
static bool rewind_mods_allow(void) {
    static bool hinted = false;
    const char* offender = NULL;
    if (openpete_mods_state_safe(&offender)) return true;
    if (!hinted) {
        hinted = true;
        OPL_WARN_F("rewind", "refused: mod \"%s\" declares no state posture (add state = \"none\" "
                   "or \"rebuildable\" to its mod.toml)", offender ? offender : "?");
    }
    return false;
}

/** @brief Report whether the rewind key is held this iteration: the key, or the --rewind-hold A:N driver once feed key A is reached.
 *
 * The driver latches at A, or at the first VBL from A on that meets
 * OPENPETE_REWIND_HOLD_PHASE (a held key does not release when the frame
 * drops below the trigger), and holds for N iterations.
 * @param opts Run options (--rewind-hold).
 * @param live The game is in live gameplay.
 * @param paused The host is paused.
 * @return true when held. */
static bool rewind_key_held(const openpete_options_t* opts, int live, bool paused) {
    static int held = 0, armed = 0;
    bool k = hc_held(OPK_REWIND, HC_GATE_MENU);
    if (opts->rewind_hold_frame >= 0 && !armed &&
        (int)openpete_feed_key_now() >= opts->rewind_hold_frame &&
        openpete_rewind_hold_phase_ready(openpete_frame))
        armed = 1;
    if (armed && held < opts->rewind_hold_iters) {
        k = true;
        held++;
        OPL_INFO_F("rewind", "hold %d/%d at f=%u (live=%d paused=%d)",
                   held, opts->rewind_hold_iters, openpete_frame, live, (int)paused);
    }
    return k;
}

/* The rewind_hold row's remaining ticks (openpete_rewind_hold_request). */
static SDL_AtomicInt s_rewind_hold_row;

int openpete_rewind_hold_request(int ticks) {
    if (ticks < 0 || (ticks > 0 && !openpete_rewind_enabled())) return -1;
    SDL_SetAtomicInt(&s_rewind_hold_row,
                     ticks > OPENPETE_REWIND_HOLD_MAX ? OPENPETE_REWIND_HOLD_MAX : ticks);
    return 0;
}

int openpete_rewind_hold_value(void) {
    const int row = SDL_GetAtomicInt(&s_rewind_hold_row);
    return hc_held(OPK_REWIND, HC_GATE_NONE) ? OPENPETE_REWIND_HOLD_MAX : row;
}

/** @brief Count one seek off the rewind_hold row's remaining ticks, unless a new request replaced the count meanwhile.
 * @param seen The count read before the seek. */
static void rewind_hold_row_consume(int seen) {
    (void)SDL_CompareAndSwapAtomicInt(&s_rewind_hold_row, seen, seen - 1);
}

/** @brief Drop every rewind keyframe when a card write reached the card file since the last call.
 *
 * Rewind keyframes carry no card and rewind never writes the card file, so
 * a keyframe taken before a committed write would restore kernel RAM whose
 * directory cache is behind the card. Called before each keyframe capture,
 * so no keyframe older than the latest committed write survives. Logs the
 * first clear of each burst of writes (writes less than 120 frames apart)
 * with the session's clear and committed-write counts. */
static void rewind_card_commit_clear(void) {
    static unsigned seen = 0, clears = 0, last_f = 0;
    const unsigned ep = openpete_memcard_commit_epoch();
    if (ep == seen) return;
    seen = ep;
    openpete_rewind_clear();
    if (clears++ == 0 || openpete_frame - last_f > 120u)
        OPL_INFO_F("rewind", "card write committed at f=%u - history cleared (%u clear(s), "
                   "%u card write(s))", openpete_frame, clears, ep);
    last_f = openpete_frame;
}

/* Rewind histories cleared by a host intervention, for the exit report. */
static unsigned long long s_rewind_intervention_clears OP_GUARDED_BY(sim) = 0;
/* The intervention epoch rewind_intervention_clear last acted on. */
static unsigned s_rewind_intervention_seen OP_GUARDED_BY(sim) = 0;

/** @brief Drop every rewind keyframe when a host intervention was recorded since the last call.
 *
 * openpete_rewind_note_intervention bumps an epoch from wherever the
 * intervention applies (a teleport, a warp, a cheat, a free camera pose or
 * engage); this call runs before each keyframe capture, so the keyframe it
 * then takes is the oldest a rewind can reach and the replay never rebuilds
 * a tick that predates the intervention. With rewind off it only follows
 * the epoch, so turning rewind on later clears nothing stale. */
static void rewind_intervention_clear(void) OP_REQUIRES(sim) {
    const char* what = NULL;
    const unsigned ep = openpete_rewind_intervention_epoch(&what);
    if (ep == s_rewind_intervention_seen) return;
    s_rewind_intervention_seen = ep;
    if (!openpete_rewind_enabled()) return;
    openpete_rewind_clear();
    s_rewind_intervention_clears++;
    OPL_INFO_F("rewind", "%s applied by f=%u - history cleared (%llu intervention clear(s))",
               what ? what : "host intervention", openpete_frame,
               s_rewind_intervention_clears);
}

bool rewind_step(const openpete_options_t* opts, bool paused, bool bk2_active) OP_REQUIRES(sim) {
    rewind_intervention_clear();
    if (!openpete_rewind_enabled()) {
        SDL_SetAtomicInt(&s_rewind_hold_row, 0);
        static bool hinted = false;
        if (!hinted && hc_held(OPK_REWIND, HC_GATE_MENU)) {
            hinted = true;
            OPL_WARN_F("rewind", "the rewind key was pressed but rewind is disabled - launch with "
                       "--rewind 30 to enable a 30 s window");
        }
        return false;
    }
    rewind_card_commit_clear();
    const int live = game_is_gameplay_live(&g_game.cpu);
    openpete_rewind_on_iter(&g_game.cpu, openpete_frame, live);
    const int row = SDL_GetAtomicInt(&s_rewind_hold_row);
    const bool held = rewind_key_held(opts, live, paused) || row > 0;
    if (!held || !live || paused || !rewind_mods_allow()) return false;
    const bool rewound = rewind_seek_one_tick(bk2_active);
    if (row > 0) rewind_hold_row_consume(row);
    return rewound;
}

bool state_ops_quiescent(const openpete_options_t* opts, bool paused, bool bk2_active) OP_REQUIRES(sim) {
    state_ops_collect(opts);
    if (!state_ops_drain()) return false;
    state_stress_step(opts);
    const bool rewound = rewind_step(opts, paused, bk2_active);
    openpete_mods_arena_page_census_tick();
    openpete_handoff_poll(&g_game.cpu);
    openpete_bugreport_replay_step(&g_game.cpu);
    openpete_bugrec_step(&g_game.cpu, rewound);
    return rewound;
}

void state_ops_report(void) OP_REQUIRES(sim) {
    OPL_INFO_F("savestate", "deferred to a final yield: %llu snapshot request(s), %llu load request(s); "
               "periodic state work sat out %llu intermediate yield(s); "
               "runahead: %llu snapshot(s) owed, %llu rollback(s) deferred, "
               "%llu adopted at another park site; "
               "left gameplay: %llu rolled back, %llu adopted; "
               "park site: loads=%llu (%llu final yield(s) waited) rewind=%llu stress=%llu "
               "runahead=%llu refused=%llu stress-abandoned=%llu",
               s_state_ops_deferred[0], s_state_ops_deferred[1], s_state_yields_skipped,
               s_ra_deferred[0], s_ra_deferred[1], s_ra_adopted,
               s_ra_left[0], s_ra_left[1],
               s_site_deferred[SITE_DEF_LOAD], s_site_wait_yields, s_site_deferred[SITE_DEF_REWIND],
               s_site_deferred[SITE_DEF_STRESS], s_site_deferred[SITE_DEF_RUNAHEAD],
               s_site_refused, s_site_abandoned);
    OPL_INFO_F("rewind", "replays short of their target: %llu; input log offset moved: %llu; "
               "presented draws replayed unseen: %llu; seen draws a later draw followed: %llu; "
               "cleared by a host intervention: %llu",
               s_rewind_short, s_rewind_log_moved, s_rewind_unseen_last, s_rewind_seen_early,
               s_rewind_intervention_clears);
    OPL_INFO_F("runahead", "presented draws built unseen: %llu (%llu on a speculation that left "
               "gameplay); seen draws a later draw followed: %llu",
               s_ra_unseen_presented, s_ra_unseen_left, s_ra_seen_early);
    openpete_bugrec_report();
    OPL_INFO_F("savestate", "deferred at an emitted loop site: %llu request(s); first at loop header "
               "0x%08X, feed key %u", s_loop_deferred, s_loop_deferred_header, s_loop_deferred_key);
}

#endif /* OPENPETE_HAS_PSYCROSS */
