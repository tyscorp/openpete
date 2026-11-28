// core/runtime/runahead.c - runahead: the live arming, the per-tick lead
// (snapshot, speculation, end of a speculation that left gameplay or was
// adopted) and its settle (rollback), the snapshot and rollback timing, the
// prediction-exactness check, and the deferral and adoption counters of a
// speculation that ends away from its snapshot's park site.

#include "runahead.h"
#include "host_state_ops.h"
#include "run_internal.h"

#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_memcard.h"
#include "openpete_mods.h"
#include "openpete_psx.h"
#include "openpete_state.h"
#include "openpete_rewind.h"
#include "openpete_time.h"
#include "game_extras.h"
#include "../rhi/rhi_gpu_audit.h"
#include "openpete_fnv1a64.h"
#include "openpete_discontinuity.h"
#include "openpete_game_runtime.h"
#include "openpete_host_trace.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_pace_log.h"
#include "bk2_feed.h"
#ifdef OPENPETE_HAS_PSYCROSS
#include "PsyX/PsyX_public.h"  /* PsyX_Pad_GetLastP1Bits */
#endif

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/* Declared in PsyCross's private src/audio/PsyX_SPUAL.h, which is not on
 * any public include path. A speculation runs the SPU speculatively; its
 * frames are committed on adoption or dropped on rollback. */
extern void PsyX_SPUAL_Speculate(int on, int commit);

int ra_pc_on(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_RA_PREDICT_CHECK");
        v = (e && *e && *e != '0') ? atoi(e) : 0;
        if (v < 0) v = 0;
    }
    return v;   /* 1 = hash check; 2 = + full led copy, dump diff ranges */
}
struct ra_pc_slot s_ra_pc[RA_PC_PENDING];
unsigned s_ra_pc_checked = 0, s_ra_pc_mismatches = 0, s_ra_pc_invalidated = 0;

bool runahead_predict_report(void) {
    if (!ra_pc_on()) return false;
    /* stderr, not the log: the roundtrip gate greps this summary. Emitted
     * whenever the check is armed, counters or not: a window that retired
     * no prediction then reads as checked=0 instead of an absent line, which
     * a harness cannot tell from a binary that never armed the check. */
    openpete_log_rawf("[ra-predict] checked=%u mismatch=%u invalidated=%u\n",
            s_ra_pc_checked, s_ra_pc_mismatches, s_ra_pc_invalidated);
    return true;
}
/* Wall time of the last runahead snapshot save, added to its restore's for
 * the Mods panel's snapshot cost. */
static op_time_t s_ra_save_ns = 0;

int ra_snapshot_save(openpete_state_mem_t* snap, const CPUState* cpu,
                            unsigned frame) {
    const op_time_t t0 = op_now();
    const int rc = openpete_state_mem_save(snap, cpu, frame);
    s_ra_save_ns = op_now() - t0;
    return rc;
}

int ra_snapshot_rollback(const openpete_state_mem_t* snap, CPUState* cpu,
                                unsigned* frame) OP_REQUIRES(sim) {
    const op_time_t t0 = op_now();
    const int rc = openpete_state_mem_load(snap, cpu, frame, 1);
    if (rc == 0) {
        openpete_mods_runahead_snapshot_note(
            (uint64_t)(s_ra_save_ns + (op_now() - t0)) / 1000u);
        openpete_mods_bus_note_rollback();
    }
    return rc;
}
void ra_card_settle(int rc) {
    if (rc == 0 || rc == -2) openpete_memcard_spec_rollback();
    else openpete_memcard_spec_adopt();
}

void ra_pc_invalidate_all(void) {
    for (int i = 0; i < RA_PC_PENDING; i++)
        if (s_ra_pc[i].valid) { s_ra_pc[i].valid = 0; s_ra_pc_invalidated++; }
}
uint64_t ra_pc_hash(void) {
    uint64_t h = openpete_fnv1a64_init();
    h = openpete_fnv1a64_update(h, openpete_psx_get_ram(), OPENPETE_PSX_RAM_SIZE);
    h = openpete_fnv1a64_update(h, openpete_psx_get_scratch(), OPENPETE_PSX_SCRATCH_SIZE);
    /* The mod arena is inside the rollback boundary: a led/committed
     * split there is the same class of misprediction as one in RAM. */
    uint32_t used[OPENPETE_ARENA_RANGES];
    (void)openpete_mods_arena_used(used);
    for (unsigned r = 0; r < OPENPETE_ARENA_RANGES; r++)
        if (used[r])
            h = openpete_fnv1a64_update(h, openpete_mods_arena_range_bytes(r), used[r]);
    return h;
}

void ra_pc_record_led(void) {
    if (!ra_pc_on()) return;
    for (int i = 0; i < RA_PC_PENDING; i++) {
        if (s_ra_pc[i].valid) continue;
        s_ra_pc[i].target = openpete_frame;
        s_ra_pc[i].hash   = ra_pc_hash();
        s_ra_pc[i].valid  = 1;
        if (ra_pc_on() < 2) return;
        if (!s_ra_pc[i].led)
            s_ra_pc[i].led = (uint8_t*)malloc(OPENPETE_PSX_RAM_SIZE + OPENPETE_PSX_SCRATCH_SIZE);
        if (!s_ra_pc[i].led) return;
        memcpy(s_ra_pc[i].led, openpete_psx_get_ram(), OPENPETE_PSX_RAM_SIZE);
        memcpy(s_ra_pc[i].led + OPENPETE_PSX_RAM_SIZE, openpete_psx_get_scratch(),
               OPENPETE_PSX_SCRATCH_SIZE);
        return;
    }
}

#ifdef OPENPETE_HAS_PSYCROSS
/* Runahead's share of s_state_ops_deferred: [0] snapshots owed to a later
 * yield (ra_owed), [1] rollbacks whose speculation ended below a non-final
 * yield. */
unsigned long long s_ra_deferred[2];
unsigned long long s_ra_adopted = 0;  /* speculations adopted: no rollback at the snapshot's park site */
unsigned long long s_ra_left[2];  /* speculations that left gameplay: [0] rolled back, [1] adopted */
unsigned long long s_ra_unseen_presented OP_GUARDED_BY(sim) = 0;
unsigned long long s_ra_seen_early OP_GUARDED_BY(sim) = 0;
unsigned long long s_ra_unseen_left OP_GUARDED_BY(sim) = 0;

/** @brief Count one runahead snapshot (kind 0) or rollback (kind 1) deferred to a final yield.
 * @param kind 0 for a snapshot, 1 for a rollback. */
static void ra_count_deferral(int kind) {
    s_ra_deferred[kind]++;
    s_state_ops_deferred[kind]++;
    count_loop_deferral();
}

int ra_note_owed(int owe, int owed) {
    if (owe && !owed) ra_count_deferral(0);
    return owe;
}

int ra_speculate_to_final(openpete_park_site_t site, int cap, int* left) {
    if (openpete_park_site_matches(&site)) return 0;
    ra_count_deferral(1);
    if (openpete_yield_final) s_site_deferred[SITE_DEF_RUNAHEAD]++;
    OPL_DEBUG_F("runahead", "speculation ended at f=%u %s; rollback deferred to the "
                "snapshot's park site", openpete_frame,
                openpete_yield_final ? "at another park site" : "below an intermediate yield");
    for (int v = 0; v < cap && !(*left && openpete_yield_final); v++) {
        game_on_frame_pre(&g_game.cpu, openpete_frame);
        g_openpete_guest_running = 1;
        openpete_fiber_switch(openpete_game_fiber);
        g_openpete_guest_running = 0;
        if (!game_is_gameplay_live(&g_game.cpu)) *left = 1;
        if (openpete_park_site_matches(&site)) return 0;
    }
    s_ra_adopted++;
    return 1;
}

void ra_end_log(int left, int adopted) {
    if (adopted) openpete_memcard_spec_adopt();
    if (left) s_ra_left[adopted ? 1 : 0]++;
    if (!adopted)
        OPL_INFO_F("runahead", "speculation left gameplay; rolled back at the snapshot's "
                   "park site (led f=%u)", openpete_frame);
    else if (left)
        OPL_INFO_F("runahead",
                   "speculation left gameplay and parked at another site than the "
                   "snapshot's; adopted the led timeline through f=%u (runahead idle "
                   "until gameplay resumes)", openpete_frame);
    else
        OPL_WARN_F("runahead",
                   "speculation reached no final yield at the snapshot's park site within "
                   "its bound; adopted the led timeline through f=%u", openpete_frame);
}


/* Live runahead depth (dev overlay). Same dirty-flag shape: the host loop
 * re-arms at its top-of-iteration quiescent point - speculation is fully
 * contained inside one ticked iteration (save -> speculate -> restore), so
 * there is never in-flight state to tear. s_runahead_live is the applied
 * depth after the mod-capability gate (may be forced 0). */
static int s_runahead_req   = 0;
static int s_runahead_dirty = 0;
static SDL_AtomicInt s_runahead_live;   /* written by the host loop, read anywhere */

/* Runahead's state across iterations, on the sim thread. */
static struct {
    int n;                       /* the armed depth, 0 when disarmed */
    openpete_state_mem_t* snap;  /* the per-tick snapshot, held while armed */
    int owed;                    /* a snapshot is owed to the next final yield */
    uint16_t pbits;              /* P1 pad bits of the previous speculation */
    unsigned char pax[4];        /* P1 analog axes of the previous speculation */
    int have;                    /* pbits and pax hold a previous speculation's input */
    int anchored;                /* the speculation counts loop-anchor ticks */
    uint32_t t0;                 /* the loop-anchor count at the speculation's start */
} s_ra;

/* Documented at the declaration in run_internal.h. */
void host_runahead_seed(const openpete_options_t* opts)
{
    s_runahead_req   = (opts->runahead > 0) ? opts->runahead : 0;
    SDL_SetAtomicInt(&s_runahead_live, 0);
    s_runahead_dirty = (s_runahead_req > 0);
}

int openpete_runahead_get(void) { return SDL_GetAtomicInt(&s_runahead_live); }

void runahead_request(int n)
{
    if (n != s_runahead_req) { s_runahead_req = n; s_runahead_dirty = 1; }
}

int runahead_requested(void) { return s_runahead_req; }

void runahead_disarm(void)
{
    if (s_ra.snap) {
        openpete_state_mem_free(s_ra.snap);
        s_ra.snap = NULL;
        OPL_INFO_F("runahead",
                   "modset changed - re-checking capability gate");
    }
    s_runahead_dirty = 1;
}

/** @brief Pass the mod capability gate for a requested depth and allocate the snapshot.
 *
 * Arm only when every enabled mod declares a runahead posture in its
 * manifest (runahead = pure | host-fx) - speculative re-execution of tick
 * hooks with undeclared host side effects would double-fire them. A refusal
 * or a failed allocation clears the request.
 * @param ra_depth The requested depth.
 * @return The depth to apply: @p ra_depth, or 0 when refused. */
static int ra_arm_gate(int ra_depth)
{
    const char* ra_offender = NULL;
    if (!openpete_mods_runahead_capable(&ra_offender)) {
        OPL_ERROR_F("runahead",
                    "disabled: mod \"%s\" declares no runahead "
                    "capability (add runahead = \"pure\" or "
                    "\"host-fx\" to its mod.toml)",
                    ra_offender ? ra_offender : "?");
        ra_depth = s_runahead_req = 0;
    } else {
        s_ra.snap = openpete_state_mem_new();
        if (!s_ra.snap) ra_depth = s_runahead_req = 0;
        else if (ra_pc_on()) {
            (void)openpete_on_guest_discontinuity(ra_pc_invalidate_all);
            OPL_INFO_F("runahead",
                       "prediction-exactness check armed");
        }
    }
    return ra_depth;
}

void runahead_apply_live(void)
{
    /* Arms once and holds the snapshot until the modset epoch disarms it. */
    if (!s_runahead_dirty) return;
    s_runahead_dirty = 0;
    int ra_depth = s_runahead_req;
    if (ra_depth > 0 && !s_ra.snap) ra_depth = ra_arm_gate(ra_depth);
    if (ra_depth != s_ra.n) {
        s_ra.n = ra_depth;
        /* The interp ring must never pair a led-timeline state with
         * a real-timeline one across the depth switch: reset it so
         * the next presents snap instead of lerping backward. */
        openpete_interp_reset();
        openpete_interp_objs_reset();
        if (s_ra.n > 0)
            OPL_INFO_F("runahead", "armed: leading presentation by "
                       "%d tick(s)", s_ra.n);
        else
            OPL_INFO_F("runahead", "disarmed");
    }
    SDL_SetAtomicInt(&s_runahead_live, s_ra.n);
}

/** @brief Format up to four bytes of a guest RAM image from @p off as hex, stopping at the end of guest RAM.
 * @param out Receives the text; 9 bytes.
 * @param ram The image, OPENPETE_PSX_RAM_SIZE bytes.
 * @param off The first byte's offset, below OPENPETE_PSX_RAM_SIZE. */
static void ra_hex4(char out[9], const uint8_t* ram, uint32_t off)
{
    const uint32_t n = OPENPETE_PSX_RAM_SIZE - off < 4u ? OPENPETE_PSX_RAM_SIZE - off : 4u;
    out[0] = '\0';
    for (uint32_t k = 0; k < n; k++) snprintf(out + 2 * k, 3, "%02X", ram[off + k]);
}

/** @brief Print the differing guest RAM ranges (as PSX addresses) and a scratchpad difference between a level-2 led copy and the committed state.
 * @param a The led copy: guest RAM followed by the scratchpad. */
static void ra_pc_dump_diff(const uint8_t* a)
{
    /* Dump differing ranges as PSX vaddrs. */
    const uint8_t* b = openpete_psx_get_ram();
    unsigned ranges = 0;
    for (uint32_t off = 0; off < OPENPETE_PSX_RAM_SIZE && ranges < 16;) {
        if (a[off] == b[off]) { off++; continue; }
        uint32_t end = off;
        while (end < OPENPETE_PSX_RAM_SIZE && end - off < 4096 &&
               (a[end] != b[end] || (end + 16 < OPENPETE_PSX_RAM_SIZE &&
                memcmp(a + end, b + end, 16) != 0)))
            end++;
        char led[9], comm[9];
        ra_hex4(led, a, off);
        ra_hex4(comm, b, off);
        fprintf(stderr, "[ra-predict]   diff 0x%08X..0x%08X led=%s comm=%s\n",
                OPENPETE_PSX_KSEG0_BASE + off, OPENPETE_PSX_KSEG0_BASE + end, led, comm);
        ranges++;
        off = end + 1;
    }
    const uint8_t* sa = a + OPENPETE_PSX_RAM_SIZE;
    const uint8_t* sb = openpete_psx_get_scratch();
    if (memcmp(sa, sb, OPENPETE_PSX_SCRATCH_SIZE) != 0)
        fprintf(stderr, "[ra-predict]   diff in SCRATCHPAD\n");
}

void runahead_check_predictions(void)
{
    /* Prediction-exactness oracle (env-gated, see ra_pc_on): an input edge
     * invalidates every pending prediction (the led branch legitimately
     * changed); then any prediction targeting THIS committed tick must
     * match the committed bytes exactly. RAM here is the completed
     * committed tick at its yield - the same phase the led hash was taken
     * at. */
    if (!ra_pc_on()) return;
    if (openpete_ra_input_edge) ra_pc_invalidate_all();
    for (int i = 0; i < RA_PC_PENDING; i++) {
        if (!s_ra_pc[i].valid) continue;
        if (s_ra_pc[i].target < openpete_frame) {
            s_ra_pc[i].valid = 0;   /* missed (mode transition) */
            continue;
        }
        if (s_ra_pc[i].target != openpete_frame) continue;
        s_ra_pc[i].valid = 0;
        s_ra_pc_checked++;
        if (s_ra_pc[i].hash != ra_pc_hash()) {
            s_ra_pc_mismatches++;
            fprintf(stderr, "[ra-predict] MISMATCH f=%u: led "
                    "prediction != committed state (a read of "
                    "host state outside the rollback "
                    "boundary - neither guest RAM nor the "
                    "mod arena - during speculation?)\n",
                    openpete_frame);
            if (ra_pc_on() >= 2 && s_ra_pc[i].led)
                ra_pc_dump_diff(s_ra_pc[i].led);
        }
    }
}

/** @brief Flag the tick as an input edge when the P1 input the speculation holds differs from the previous speculation's.
 *
 * The led branch changes exactly when the input we speculate with differs
 * from last tick's. Flag the tick so presentation consumers can rebase
 * their interp at the last SHOWN state instead of snapping to the corrected
 * branch (measured: R2 release at N=2 showed a -88u/present camera
 * reversal; N ticks of mispredicted pan un-shown). */
static void ra_input_edge_update(void)
{
    uint16_t rb = PsyX_Pad_GetLastP1Bits();
    unsigned char rax[4];
    PsyX_Pad_GetLastP1Analog(rax);
    openpete_ra_input_edge =
        (s_ra.have && (rb != s_ra.pbits ||
         memcmp(rax, s_ra.pax, 4) != 0)) ? 1 : 0;
    s_ra.pbits = rb;
    memcpy(s_ra.pax, rax, 4);
    s_ra.have = 1;
}

/* The draws of the iteration in progress: the committed tick's and its
 * speculation's, for openpete_draw_unseen. */
static draw_run_t s_ra_run OP_GUARDED_BY(sim) = { 0u, DRAW_RUN_NONE, 2u, 0, 0, 0u };

/** @brief Report whether a speculation may follow the committed tick: runahead is armed with its snapshot and gameplay is live.
 * @return 1 when it may, else 0. */
static int ra_live(void) OP_REQUIRES(sim)
{
    return s_ra.n > 0 && s_ra.snap && game_is_gameplay_live(&g_game.cpu);
}

/* Documented at the declaration in openpete_fiber.h. */
int openpete_draw_unseen(void) OP_REQUIRES(sim)
{
    if (openpete_replaying) return openpete_rewind_draw_unseen();
    if (!openpete_speculative) return draw_run_note(&s_ra_run, ra_live());
    if (s_ra.anchored)
        return draw_run_note(&s_ra_run,
                             openpete_loop_anchor_ticks() - s_ra.t0 < (uint32_t)s_ra.n);
    return draw_run_note(&s_ra_run, draw_run_frame_unseen(&s_ra_run, openpete_frame));
}

/** @brief Run the speculated VBLs: runahead_n game ticks counted by the loop anchor, or 2N yields without one.
 *
 * Ends at the next final yield once the tick count is reached (where the
 * rollback may load). The bound caps a speculation that never reaches one.
 * The anchor count at the start and the last yield's frame are kept for
 * openpete_draw_unseen.
 * @param left Set when the speculation left gameplay (a load, a cutscene). */
static void ra_speculate_ticks(int* left)
{
    s_ra.t0 = openpete_loop_anchor_ticks();
    s_ra.anchored = openpete_game_runtime()->loop_anchor_pc != 0u;
    draw_run_aim(&s_ra_run, openpete_frame + 2u * (unsigned)s_ra.n);
    for (int v = 0; v < (s_ra.anchored ? 8 * s_ra.n + 16 : 2 * s_ra.n); v++) {
        if (s_ra.anchored && v > 0 && openpete_yield_final &&
            openpete_loop_anchor_ticks() - s_ra.t0 >= (uint32_t)s_ra.n)
            break;
        unsigned bf = openpete_frame;
        game_on_frame_pre(&g_game.cpu, openpete_frame);
        g_openpete_guest_running = 1;
        openpete_fiber_switch(openpete_game_fiber);
        g_openpete_guest_running = 0;
        if (openpete_frame == bf) break;   /* safety */
        /* Speculation left gameplay (a load, a cutscene):
         * stop counting ticks. The rollback still loads when the
         * transition waits at the snapshot's psyq_VSync site. */
        if (!game_is_gameplay_live(&g_game.cpu)) {
            *left = 1;
            break;
        }
    }
}

/** @brief Speculate from the snapshot just saved: held input, audible SPU muted, external side effects gated by openpete_speculative.
 *
 * Host flags the guest's draw thunks mutate are part of the rolled-back
 * state: a speculative DrawOTag leaking present_pending fired EXTRA
 * canonical presents (measured: canon_tick cadence doubled under runahead -
 * every tick-keyed instrument sampled the wrong sim moments), so a
 * speculation that is not adopted restores them.
 * @param left Set when the speculation left gameplay.
 * @return Nonzero when the speculation was adopted as the timeline. */
static int ra_speculate(int* left)
{
    int      ra_pp    = openpete_present_pending;
    unsigned ra_prims = openpete_primitives_drawn;
    PsyX_SPUAL_Speculate(1, 0);
    openpete_speculative = 1;
    /* Speculated ticks run inside this VBL's bracket but are
     * not this VBL: their pad reads must not be measured
     * against its exec_start. */
    openpete_pace_vbl_suspend(1);
    ra_speculate_ticks(left);
    /* The rollback loads only at a final yield. */
    const int ra_adopted = ra_speculate_to_final(openpete_state_mem_site(s_ra.snap),
                                                 8 * s_ra.n + 16, left);
    openpete_speculative = 0;
    openpete_pace_vbl_suspend(0);
    /* An adopted speculation's pad writes and consumer
     * entries reach the recorder; a rolled-back one's are
     * run again by the committed ticks. */
    bk2_feed_speculation_end(ra_adopted);
    /* An adopted speculation is the timeline: its frames
     * are fed. A rolled-back one is rendered again by the
     * committed ticks. */
    PsyX_SPUAL_Speculate(0, ra_adopted);
    if (!ra_adopted) {
        openpete_present_pending  = ra_pp;
        openpete_primitives_drawn = ra_prims;
    }
    /* Record the led prediction while RAM is still the
     * speculation head (the rollback happens in runahead_settle,
     * after the interp captures); openpete_frame is the led
     * frame here. */
    if (!*left && !ra_adopted) ra_pc_record_led();
    return ra_adopted;
}

/** @brief End a speculation that left gameplay or was adopted: no rollback follows, pending predictions die and the presentation snaps.
 *
 * A speculation that left gameplay and was not adopted leads into a
 * transition the committed ticks reach N ticks later: it is rolled back
 * here, so the capture takes the committed state as on a tick without
 * speculation. An adopted speculation's VBLs are the real timeline.
 * @param left Nonzero when the speculation left gameplay.
 * @param adopted Nonzero when it was adopted. */
static void ra_end_away(int left, int adopted) OP_REQUIRES(sim)
{
    if (left && !adopted) {
        unsigned ra_lf = openpete_frame;
        const int ra_rc = openpete_state_mem_load(s_ra.snap, &g_game.cpu, &ra_lf, 1);
        if (ra_rc == 0)
            openpete_frame_set(ra_lf);
        ra_card_settle(ra_rc);
    }
    if (!left && !adopted) return;
    /* Either way the rollback in runahead_settle is skipped, predictions
     * pending against the led branch die, and the presentation drops its
     * pairs and snaps (a fade is starting), so no lerp pairs a led state
     * with the committed ticks that follow. */
    ra_pc_invalidate_all();
    openpete_ra_input_edge = 0;
    /* Presentation-only reset (NOT the guest-discontinuity
     * broadcast - the timeline is continuous forward, and
     * the broadcast would finalize an in-progress bk2
     * recording). */
    openpete_interp_reset();
    openpete_interp_objs_reset();
    ra_end_log(left, adopted);
    host_trace_ra_end(left, adopted);
}

runahead_lead_t runahead_lead(void) OP_REQUIRES(sim)
{
    runahead_lead_t r = { RA_NONE, false };
    const int ra_want = ra_live();
    int ra_left = 0;      /* the speculation left gameplay */
    int ra_adopted = 0;   /* the snapshot's park site did not recur: no rollback */
    bool ra_speculated = false;
    /* The snapshot is taken at a final yield only: a tick whose
     * draw is first seen at an intermediate yield of its wait
     * owes its speculation to the wait's final yield. */
    if (!ra_want) openpete_ra_input_edge = 0;
    const int ra_due = ra_want && (openpete_present_pending || s_ra.owed);
    const int ra_now = ra_due && openpete_yield_final;
    s_ra.owed = ra_note_owed(ra_due && !ra_now, s_ra.owed);
    if (ra_now &&
        ra_snapshot_save(s_ra.snap, &g_game.cpu,
                         openpete_frame) == 0) {
        ra_input_edge_update();
        runahead_check_predictions();
        ra_adopted = ra_speculate(&ra_left);
        ra_speculated = true;
        host_trace_ra(HT_RA_SPECULATED);
    }
    ra_end_away(ra_left, ra_adopted);
    /* The scene the present shows is complete unless a speculation is owed. */
    if (!s_ra.owed && draw_run_close(&s_ra_run, &s_ra_unseen_presented, &s_ra_seen_early)) {
        s_ra_unseen_left += (unsigned long long)(ra_left != 0);
        OPL_DEBUG_F("runahead", "presented draw at f=%u was predicted unseen (speculated=%d "
                    "left=%d adopted=%d anchor ticks=%u)", openpete_frame, ra_speculated, ra_left,
                    ra_adopted, openpete_loop_anchor_ticks() - s_ra.t0);
    }
    r.skip_capture = (ra_want && !ra_speculated && !ra_left) || ra_adopted;
    if (ra_speculated)
        r.outcome = ra_adopted ? RA_ADOPTED : ra_left ? RA_LEFT : RA_SPECULATED;
    return r;
}

void runahead_settle(runahead_lead_t r) OP_REQUIRES(sim)
{
    /* Captures done at the led head - roll back to the committed tick
     * before anything else observes RAM. */
    if (r.outcome != RA_SPECULATED) return;
    unsigned ra_lf = openpete_frame;
    const int ra_rc = ra_snapshot_rollback(s_ra.snap, &g_game.cpu, &ra_lf);
    ra_card_settle(ra_rc);
    if (ra_rc == 0) {
        openpete_frame_set(ra_lf);
        /* Committed-timeline regions (g_Camera): replace
         * their led bytes in the ring's newest slot with
         * the just-restored committed state, so those
         * regions pair a branch-free timeline and can
         * never show (or snap back from) a misprediction. */
        openpete_interp_recapture_committed();
    }
}

#endif /* OPENPETE_HAS_PSYCROSS */
