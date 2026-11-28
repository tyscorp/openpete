// games/common/features/levelwarp_ui.c - the dev-overlay warp: the
// dropdown, the Warp button, the click queue, the scripted driver and the
// in-flight state machine. See levelwarp_ui.h for the contract.

#include "features/levelwarp_ui.h"

#include "game_extras.h"    /* game_set_skip_to_level (title-screen route) */
#include "openpete_bugrec.h" /* openpete_bugrec_warp, openpete_bugrec_hold */
#include "openpete_bugreport_replay.h" /* openpete_bugreport_replay_warp_take */
#include "openpete_fiber.h" /* openpete_speculative, openpete_frame */
#include "openpete_freecam.h"
#include "openpete_log.h"
#include "openpete_rewind.h"   /* openpete_rewind_note_intervention */
#include "openpete_surface.h"
#include "psx_runtime_internal.h" /* CPUState + its read callbacks */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Backstop for a warp that never resolves. Every phase ends on a state the
 * game itself reaches, so this only fires if the game parks somewhere
 * unanticipated; it must not double as a deadline. Counted in calls (one per
 * sim yield): a CD load plus fly-in can run past 3600 in a headless
 * free-running session, which is why no phase leans on it. */
#define WARP_STUCK_CALLS 20000u

/* The registered warps, for the row callbacks, which receive only the row. */
static levelwarp_t* s_warps[2];

/** @brief Find the warp a dev-overlay row belongs to, by the row's id.
 *
 * @param k The row.
 * @return The warp whose templates carry the id; the first warp when none does. */
static levelwarp_t* warp_of(const knob_desc_t* k) {
    for (size_t i = 0; i < sizeof s_warps / sizeof s_warps[0]; i++) {
        const levelwarp_t* w = s_warps[i];
        if (w && (strcmp(k->id, w->game->rows[0].id) == 0 ||
                  strcmp(k->id, w->game->rows[1].id) == 0))
            return s_warps[i];
    }
    return s_warps[0];
}

/** @brief Report whether the warp has a request queued or a warp in flight, the Warp button's second gate.
 *
 * @param w The warp.
 * @return Non-zero while a click is queued or the phase is not idle. */
static int warp_busy(const levelwarp_t* w) {
    return w->pending || w->phase != LEVELWARP_IDLE;
}

/* ---- dev-overlay rows ------------------------------------------------- */

/** @brief Read the dropdown row's selected index.
 *
 * @param k The row.
 * @return The index into the warp's target table. */
static double sel_get(const knob_desc_t* k) { return (double)warp_of(k)->sel; }

/** @brief Select a dropdown entry and stop the dropdown following the current target.
 *
 * @param k The row.
 * @param v The index into the warp's target table.
 * @return 0 when applied; -1 when the index is outside the table. */
static int sel_set(knob_desc_t* k, double v) {
    levelwarp_t* w = warp_of(k);
    int idx = (int)v;
    if (idx < 0 || idx >= w->count) return -1;
    w->sel         = idx;
    w->sel_touched = 1;
    return 0;
}

/** @brief Gate the Warp button on a usable route, an accepted target and no warp in progress.
 *
 * @param k The row.
 * @return 1 when the button may fire, else 0. */
static double go_ready(const knob_desc_t* k) {
    const levelwarp_t* w = warp_of(k);
    return (w->route != LEVELWARP_ROUTE_NONE && !w->refused && !warp_busy(w))
        ? 1.0 : 0.0;
}

/** @brief Queue a warp to the selected target when the Warp button is pressed.
 *
 * Snapshots the selected target number, since the dropdown can move before
 * the tick drains the queue.
 * @param k The row.
 * @param v The press value; unused.
 * @return 0 when queued; -1 when the button is gated. */
static int go_fire(knob_desc_t* k, double v) {
    levelwarp_t* w = warp_of(k);
    (void)v;
    if (go_ready(k) == 0.0) return -1;
    w->pending_id = w->game->table->levels[w->sel].number;
    w->pending    = 1;
    return 0;
}

/** @brief Write the status text shown beside the Warp button.
 *
 * Reads queued while a click waits; "loading..." while a warp is in
 * flight; else a note for the title and demo routes, the refusal reason or
 * an empty string for the level route, or with no route the pause-menu
 * reason or the game's no-route text.
 * @param k The row.
 * @param buf Receives the text.
 * @param cap Size of @p buf in bytes. */
static void go_status(const knob_desc_t* k, char* buf, size_t cap) {
    const levelwarp_t* w = warp_of(k);
    const levelwarp_game_t* g = w->game;
    const char* s;
    if (w->pending)                             s = "queued";
    else if (w->phase != LEVELWARP_IDLE)        s = "loading...";
    else if (w->route == LEVELWARP_ROUTE_TITLE) s = "from title or menu (resets progress)";
    else if (w->route == LEVELWARP_ROUTE_DEMO)  s = "ends the demo (resets progress)";
    else if (w->route == LEVELWARP_ROUTE_LEVEL) s = w->refused ? w->refused : "";
    else if (w->refused)                        s = w->refused;
    else s = g->none_status ? g->none_status : "not in a level";
    snprintf(buf, cap, "%s", s);
}

/* The level warp's rows, as templates: register_rows fills in the
 * dropdown's display names and the value plumbing, and levelwarp_surface
 * hands the templates to the surface dump as they are. */
const knob_desc_t levelwarp_level_rows[2] = {
    { .id = "warp_level", .label = "Level warp", .section = "Gameplay",
      .help = "Destination for the Warp button. Follows the level you "
              "are in until you pick something.",
      .type = KNOB_ENUM, .mut = KNOB_LIVE,
      .default_note = "the level the player is in",
      .replay = KNOB_REPLAY_FOLLOW },
    { .id = "warp_go", .label = "Warp to level", .section = "Gameplay",
      .help = "Load the selected level through the game's own level "
              "transition. From the title screen, its menu or the attract "
              "demo, and on the games whose transition goes by way of the "
              "title, that is a fresh session in the chosen level and "
              "progress and lives reset, as with --skip-to-level.",
      .type = KNOB_ACTION, .mut = KNOB_LIVE, .action_label = "Warp" },
};

/** @brief Add the warp's dropdown and Warp rows to the dev overlay.
 *
 * @param w The warp. */
static void register_rows(levelwarp_t* w) {
    knob_desc_t row = w->game->rows[0];
    row.enum_values = w->display;
    row.enum_count  = w->count;
    row.get_num     = sel_get;
    row.set_num     = sel_set;
    (void)openpete_knob_add(&row);

    row         = w->game->rows[1];
    row.get_num = go_ready;
    row.set_num = go_fire;
    row.get_str = go_status;
    (void)openpete_knob_add(&row);
}

void levelwarp_surface(struct openpete_surface_sink* sink,
                       const knob_desc_t* rows) {
    openpete_surface_row(sink, &rows[0]);
    openpete_surface_row(sink, &rows[1]);
}

/* ---- scripted driver --------------------------------------------------- */

/* <test_env>=<vbl>:<target> queues the same request the Warp button does,
 * at the given VBL counter. The warp is a multi-tick ride through the game's
 * own state machine, so firing it from a script is what makes it testable
 * from a script. A harness variable, not a knob registry row:
 *   OPENPETE_LEVELWARP_TEST=2200:tree_tops build/linux-release/openpete --skip-to-level artisans
 */

/** @brief Arm the scripted warp from the game's test variable.
 *
 * The variable reads <vbl>:<target>. The text after the colon, truncated to
 * 31 bytes, is resolved against the warp's table; an unset or malformed
 * variable, or a target that does not resolve, leaves the trigger disarmed.
 * @param w The warp. */
static void test_trigger_init(levelwarp_t* w) {
    const char* spec = w->game->test_env ? getenv(w->game->test_env) : NULL;
    const char* colon;
    char        lvl[32];
    size_t      n;

    w->test_frame = -1;
    if (!spec || !spec[0]) return;
    colon = strchr(spec, ':');
    if (!colon) return;
    for (n = 0; colon[1 + n] && n < sizeof(lvl) - 1; n++) lvl[n] = colon[1 + n];
    lvl[n] = '\0';

    w->test_id = game_level_resolve(w->game->table, lvl);
    if (w->test_id < 0) { w->test_id = 0; return; }
    w->test_frame = atoi(spec);
    OPL_INFO_F(w->game->log_tag, "harness: warp to %s %d at frame %d",
               w->game->unit, w->test_id, w->test_frame);
}

/** @brief Queue the scripted warp once the frame counter reaches its trigger.
 *
 * Fires once, then disarms.
 * @param w The warp. */
static void test_trigger_tick(levelwarp_t* w) {
    if (w->test_frame < 0 || (int)openpete_frame < w->test_frame) return;
    w->test_frame = -1;
    w->pending_id = w->test_id;
    w->pending    = 1;
}

/** @brief Queue the bundle's recorded request of this warp due at the current tick, under --bug-report-replay.
 *
 * The recording logged the request at the tick its queue drained; queued
 * here, before the drain, it drains on that tick again.
 * @param w The warp. */
static void replay_trigger_tick(levelwarp_t* w) {
    int target = 0;
    if (!openpete_bugreport_replay_warp_take(w->game->rows[1].id, &target)) return;
    w->pending_id = target;
    w->pending    = 1;
}

/* ---- entry points ------------------------------------------------------ */

void levelwarp_register(levelwarp_t* w) {
    const game_level_table_t* t = w->game->table;
    w->count = (int)(t->count < 64 ? t->count : 64);
    for (int i = 0; i < w->count; i++) w->display[i] = t->levels[i].display;
    levelwarp_reset(w);
    w->sel = w->sel_touched = w->pending = 0;
    w->route   = LEVELWARP_ROUTE_NONE;
    w->refused = NULL;
    w->title_hold = 0;
    w->slot    = s_warps[0] && s_warps[0] != w ? 1 : 0;
    s_warps[w->slot] = w;
    register_rows(w);
    test_trigger_init(w);
}

void levelwarp_reset(levelwarp_t* w) {
    w->phase  = LEVELWARP_IDLE;
    w->target = 0;
    w->ticks  = 0;
}

void levelwarp_abort(levelwarp_t* w, const char* why) {
    OPL_INFO_F(w->game->log_tag, "aborted (%s)", why);
    levelwarp_reset(w);
}

void levelwarp_called(levelwarp_t* w) {
    w->phase = LEVELWARP_LEAVING;
    w->ticks = 0;
}

/** @brief Read the target number the player is at.
 *
 * @param g The game.
 * @param cpu The guest CPU state.
 * @return g->current(cpu) when the game supplies it, else the word at
 *         g->current_word. */
static uint32_t current_of(const levelwarp_game_t* g, CPUState* cpu) {
    return g->current ? g->current(cpu) : cpu->read_word(g->current_word);
}

/** @brief Pick the route the live guest state allows.
 *
 * The title route when title_word reads title_value; else none unless
 * live() holds and state_word reads 0 (a pause menu or a transition over the
 * resident level closes both remaining routes); then the in-level route when
 * demo_word reads 0, the demo route when it reads nonzero and the game
 * supplies demo_end, else none. The free camera does not block the in-level
 * route: drain disengages it before the warp starts.
 * @param g The game.
 * @param cpu The guest CPU state.
 * @return A LEVELWARP_ROUTE_* value. */
static int route_of(const levelwarp_game_t* g, CPUState* cpu) {
    if (g->title_word && cpu->read_word(g->title_word) == g->title_value)
        return LEVELWARP_ROUTE_TITLE;
    if (!g->live(cpu)) return LEVELWARP_ROUTE_NONE;
    if (g->state_word && cpu->read_word(g->state_word) != 0u)
        return LEVELWARP_ROUTE_NONE;
    if (!g->demo_word || cpu->read_word(g->demo_word) == 0u)
        return LEVELWARP_ROUTE_LEVEL;
    return g->demo_end ? LEVELWARP_ROUTE_DEMO : LEVELWARP_ROUTE_NONE;
}

/** @brief Name why the Warp button cannot fire toward a target on the current route.
 *
 * On the in-level route, the game's target_refused reason. With no route,
 * "close the pause menu first" while state_word reads the game's
 * pause_value and demo_word reads 0. NULL otherwise.
 * @param w The warp; its route is current.
 * @param cpu The guest CPU state.
 * @param target The target number.
 * @return The reason, or NULL. */
static const char* refusal_of(const levelwarp_t* w, CPUState* cpu, int target) {
    const levelwarp_game_t* g = w->game;
    if (w->route == LEVELWARP_ROUTE_LEVEL)
        return g->target_refused ? g->target_refused(cpu, target) : NULL;
    if (w->route != LEVELWARP_ROUTE_NONE || !g->state_word || !g->pause_value)
        return NULL;
    if (cpu->read_word(g->state_word) != g->pause_value) return NULL;
    if (g->demo_word && cpu->read_word(g->demo_word) != 0u) return NULL;
    return "close the pause menu first";
}

/** @brief Hand a target to the --skip-to-level machinery, the title and demo routes' common half.
 *
 * The machinery induces the title's demo-timeout transition at its next
 * window, which also makes the progress and lives reset a fresh session
 * needs.
 * @param g The game.
 * @param target The target number. */
static void title_route(const levelwarp_game_t* g, int target) {
    if (g->title_start) g->title_start(target);
    else                game_set_skip_to_level(target);
}

/** @brief Start a queued request through the route it drained under.
 *
 * A level-route warp that starts records a rewind intervention, so a rewind
 * never replays across it; the title and demo routes start outside
 * gameplay, where the rewind history is already empty.
 * @param w The warp.
 * @param cpu The guest CPU state.
 * @param target The target number. */
static void drain(levelwarp_t* w, CPUState* cpu, int target) {
    const levelwarp_game_t* g = w->game;
    if (w->route == LEVELWARP_ROUTE_TITLE) {
        title_route(g, target);
        w->title_hold = 1;
        OPL_INFO_F(g->log_tag, "queued title-screen boot into %s %d", g->unit, target);
    } else if (w->route == LEVELWARP_ROUTE_DEMO) {
        /* The demo's own end returns the game to the title, where the
         * armed title route takes the target. */
        g->demo_end(cpu);
        title_route(g, target);
        w->title_hold = 1;
        OPL_INFO_F(g->log_tag, "ending the attract demo for %s %d", g->unit, target);
    } else if (w->route == LEVELWARP_ROUTE_LEVEL && !w->refused) {
        /* The free camera freezes the player update and replaces the camera
         * follow, both of which the game's exit sequence runs through.
         * Disengaging on the game tick, before the start hook, leaves this
         * tick's player update and follow to run unfrozen, as the teleport's
         * disengage does. */
        if (openpete_freecam_active()) {
            openpete_freecam_toggle();
            OPL_INFO_F(g->log_tag, "%s", "free camera disengaged for the warp");
        }
        w->target = (uint32_t)target;
        const int ph = g->start(w, cpu, target);
        if (ph == LEVELWARP_IDLE) { levelwarp_reset(w); return; }
        openpete_rewind_note_intervention("warp");
        w->phase = (uint32_t)ph;
        w->ticks = 0;
    } else {
        OPL_INFO_F(g->log_tag, "%s", "ignored (no route from this state)");
    }
}

/** @brief Run the front half: refresh the route, follow the dropdown, and drain a queued request.
 *
 * @param w The warp.
 * @param cpu The guest CPU state.
 * @return 1 when the in-flight half must not run this tick: a speculative
 *         tick, a tick holding a request behind an in-flight warp, or a tick
 *         that drained a request; else 0. */
static int ui_tick(levelwarp_t* w, CPUState* cpu) {
    const levelwarp_game_t* g = w->game;
    /* Runahead: a speculative tick's guest writes are rolled back, so
     * consuming the click here would drop the warp. */
    if (openpete_speculative) return 1;
    /* The harness request queues before anything reads `pending`, so it
     * lands where a click lands: set between ticks, drained on the next. */
    test_trigger_tick(w);
    replay_trigger_tick(w);

    w->route   = route_of(g, cpu);
    /* The title and demo routes leave the game's level skip armed with the
     * warp idle; that host state lasts until gameplay in a level. */
    if (w->title_hold && (w->route == LEVELWARP_ROUTE_LEVEL || ++w->title_hold > WARP_STUCK_CALLS))
        w->title_hold = 0;
    w->refused = refusal_of(w, cpu, g->table->levels[w->sel].number);

    if (!w->sel_touched && w->phase == LEVELWARP_IDLE) {
        int idx = game_level_index(g->table, (int)current_of(g, cpu));
        if (idx >= 0) w->sel = idx;
    }

    if (!w->pending) return 0;
    if (w->phase != LEVELWARP_IDLE) return 1;   /* drain when idle */
    w->pending = 0;
    openpete_bugrec_warp(g->rows[1].id, w->pending_id);
    w->refused = refusal_of(w, cpu, w->pending_id);
    drain(w, cpu, w->pending_id);
    return 1;
}

/** @brief Log the arrival, then run the game's arrival hook.
 *
 * @param w The warp.
 * @param cpu The guest CPU state. */
static void arrive(levelwarp_t* w, CPUState* cpu) {
    const levelwarp_game_t* g = w->game;
    const uint32_t landed = current_of(g, cpu);
    const unsigned vbl = (unsigned)SDL_GetAtomicInt(&openpete_vbl);
    if (landed != w->target)
        OPL_INFO_F(g->log_tag, "landed in %s %u, wanted %u, at vbl=%u", g->unit,
                   (unsigned)landed, (unsigned)w->target, vbl);
    else
        OPL_INFO_F(g->log_tag, "arrived in %s %u at vbl=%u", g->unit,
                   (unsigned)landed, vbl);
    const int ph = g->on_arrival ? g->on_arrival(w, cpu, landed) : LEVELWARP_IDLE;
    if (ph == LEVELWARP_IDLE) { levelwarp_reset(w); return; }
    w->phase = (uint32_t)ph;
    w->ticks = 0;
}

/** @brief Run the tick's front half, then advance an in-flight warp one step.
 *
 * @param w The warp.
 * @param cpu The guest CPU state. */
static void warp_tick(levelwarp_t* w, CPUState* cpu) {
    const levelwarp_game_t* g = w->game;
    if (ui_tick(w, cpu)) return;
    if (w->phase == LEVELWARP_IDLE || w->phase == LEVELWARP_QUEUED) return;
    if (++w->ticks > WARP_STUCK_CALLS) { levelwarp_abort(w, "stuck"); return; }

    /* The game has the wheel: LEAVING writes only through the game's hook,
     * LOADING writes nothing. The level the warp left is still live on the
     * tick its transition is entered, so arrival is read only once it has
     * gone, however long the CD load takes. */
    if (w->phase == LEVELWARP_LEAVING) {
        const int ph = g->leaving ? g->leaving(w, cpu)
                     : g->live(cpu) ? LEVELWARP_LEAVING : LEVELWARP_LOADING;
        if (ph == LEVELWARP_IDLE) { levelwarp_reset(w); return; }
        if (ph == LEVELWARP_LOADING) w->ticks = 0;
        w->phase = (uint32_t)ph;
        return;
    }
    if (g->arrived ? g->arrived(cpu) : g->live(cpu)) arrive(w, cpu);
}

void levelwarp_on_tick(levelwarp_t* w, CPUState* cpu) {
    warp_tick(w, cpu);
    /* The phase, the target and the games' own warp state live outside
     * every savestate, so no bug-report anchor is taken while they are set. */
    if (!openpete_speculative)
        openpete_bugrec_hold((unsigned)w->slot, w->phase != LEVELWARP_IDLE || w->title_hold);
}
