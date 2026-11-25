// games/common/features/level_skip.c - the shared --skip-to-level driver;
// see level_skip.h.

#include "features/level_skip.h"

#include "openpete_fiber.h"
#include "openpete_skip.h"
#include "psx_runtime_internal.h" /* CPUState + its read callbacks */

#include <stdio.h>

typedef enum {
    SKIP_IDLE = 0,
    SKIP_ARMED,    /* waiting for the title's induction window */
    SKIP_SEEDED,   /* induced; waiting for the loader to take the target */
    SKIP_PICKED,   /* the loader took the target; holding the demo flag clear */
    SKIP_REACHED,  /* the hold ended; level_skip_picked still reports the pick */
} skip_phase_t;

static int          s_target = 0;
static skip_phase_t s_phase  = SKIP_IDLE;
/* Present fast-forward stop cap, set relative to the first release check so
 * a runtime warp (armed tens of thousands of frames in) is not cleared at
 * once. */
static uint32_t     s_ffwd_deadline = 0;
/* The game.state word at the previous release check, for the re-arm edge. */
static uint32_t     s_prev_state = 0;

/** @brief Return the VBL counter every log line prints.
 *
 * @return openpete_vbl. */
static uint32_t log_vbl(void) {
    return (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
}

void level_skip_ffwd_arm(const char* until) {
    if (!openpete_skip_ffwd_windowed) return;
    openpete_skip_present_until_level = 1;
    s_ffwd_deadline = 0;
    if (until)
        fprintf(stderr, "[skip-to-level] fast-forwarding presents until %s\n", until);
}

void level_skip_set(const level_skip_game_t* g, int number) {
    char until[40];
    (void)g;
    s_target = number > 0 ? number : 0;
    s_phase  = number > 0 ? SKIP_ARMED : SKIP_IDLE;
    if (number <= 0) return;
    snprintf(until, sizeof until, "level %d loads", number);
    level_skip_ffwd_arm(until);
}

int level_skip_target(const level_skip_game_t* g) {
    (void)g;
    return s_target;
}

int level_skip_picked(void) {
    return s_phase == SKIP_PICKED || s_phase == SKIP_REACHED;
}

/** @brief Return the first watching release watch.
 *
 * @param g The game.
 * @return The watch, or NULL when none is watching. */
static const level_skip_watch_t* watching(const level_skip_game_t* g) {
    for (size_t i = 0; i < g->n_watches; i++)
        if (g->watches[i].watching()) return &g->watches[i];
    return NULL;
}

/** @brief Re-set the fail-safe deadline when unset or on the game's re-arm edge.
 *
 * @param g The game.
 * @param cpu The guest CPU state.
 * @param frame_counter The openpete_frame counter. */
static void deadline_tick(const level_skip_game_t* g, CPUState* cpu,
                          uint32_t frame_counter) {
    int edge = 0;
    if (g->rearm_word) {
        const uint32_t state = cpu->read_word(g->rearm_word);
        edge = state == g->rearm_value && s_prev_state != g->rearm_value;
        s_prev_state = state;
    }
    if (!s_ffwd_deadline || edge)
        s_ffwd_deadline = frame_counter + openpete_skip_failsafe_vbls();
}

/** @brief Release the skip on its release edge or on the fail-safe deadline.
 *
 * Runs while a present fast-forward or a watch is armed. The first watching
 * watch's release predicate decides; with none watching, the picked phase's
 * presentable edge, or its playable edge when the game supplies none. The
 * release line names the engagement values the watch reads back. Either
 * release clears openpete_skip_present_until_level, zeroes the deadline and
 * finishes every watching watch. The watches run
 * headless too, where there is no fast-forward, so a harness reads the same
 * lines.
 * @param g The game.
 * @param cpu The guest CPU state.
 * @param frame_counter The openpete_frame counter. */
static void release_tick(const level_skip_game_t* g, CPUState* cpu,
                         uint32_t frame_counter) {
    const level_skip_watch_t* w = watching(g);
    if (!openpete_skip_present_until_level && !w) return;
    deadline_tick(g, cpu, frame_counter);
    char eng[128] = "";
    int (*const edge)(CPUState*) = g->presentable ? g->presentable : g->playable;
    const int playable = w ? w->released(cpu, eng, sizeof eng)
                           : s_phase == SKIP_PICKED && edge(cpu);
    if (playable) {
        fprintf(stderr, "[skip-to-level] level playable - presents resume "
                        "at vbl=%u key=%u%s\n", log_vbl(), openpete_feed_key_now(), eng);
    } else if (frame_counter > s_ffwd_deadline) {
        fprintf(stderr, "[skip-to-level] fail-safe release (no playable edge) "
                        "at vbl=%u key=%u\n", log_vbl(), openpete_feed_key_now());
    } else {
        return;
    }
    openpete_skip_present_until_level = 0;
    s_ffwd_deadline = 0;
    for (size_t i = 0; i < g->n_watches; i++)
        if (g->watches[i].watching()) g->watches[i].finish();
}

/** @brief Advance the armed and seeded phases one tick.
 *
 * @param g The game.
 * @param cpu The guest CPU state. */
static void advance(const level_skip_game_t* g, CPUState* cpu) {
    if (s_phase == SKIP_ARMED) {
        const int r = g->induce(cpu, s_target, log_vbl());
        if (r > 0) {
            s_phase = SKIP_SEEDED;
        } else if (r < 0) {
            /* A refused skip never induces, so the playable-edge release
             * cannot fire: drop the fast-forward now. */
            s_phase  = SKIP_IDLE;
            s_target = 0;
            openpete_skip_present_until_level = 0;
            s_ffwd_deadline = 0;
        }
    } else if (s_phase == SKIP_SEEDED) {
        if (g->picked && !g->picked(cpu, s_target)) return;
        s_phase = SKIP_PICKED;
        if (g->picked)
            fprintf(stderr, "[skip-to-level] loader took level=%d at vbl=%u\n",
                    s_target, log_vbl());
        g->restore(cpu);
    }
}

void level_skip_on_tick(const level_skip_game_t* g, CPUState* cpu,
                        uint32_t frame_counter) {
    advance(g, cpu);
    /* The hold runs on the playable tick too, and the release check reads
     * the picked phase, so the hold ends after both: on the playable edge
     * into SKIP_REACHED, or, when the game left the level first, disarmed. */
    skip_phase_t next = s_phase;
    if (s_phase == SKIP_PICKED)
        next = !g->hold(cpu) ? SKIP_IDLE
             : g->playable(cpu) ? SKIP_REACHED : SKIP_PICKED;
    release_tick(g, cpu, frame_counter);
    if (next == s_phase) return;
    s_phase  = next;
    s_target = 0;
    fprintf(stderr, "[skip-to-level] demo-mode hold ends at vbl=%u\n",
            log_vbl());
}

void level_skip_arm_cutscene(const level_skip_game_t* g, int id) {
    if (id <= 0) return;
    const int source = g->cutscene_source(id);
    if (source < 0 && g->cutscene_default_level <= 0) return;
    const int level = level_skip_target(g);
    if (source >= 0 && level > 0 && level != source) {
        char from[24] = "the title";
        if (source > 0) snprintf(from, sizeof from, "level %d", source);
        fprintf(stderr, "openpete: --skip-to-cutscene: this cutscene starts "
                        "from %s, not --skip-to-level %d; refused\n", from, level);
        return;
    }
    g->cutscene_arm(id);
    if (source == 0) { level_skip_ffwd_arm(NULL); return; }
    if (level <= 0 && (!g->cutscene_rides || g->cutscene_rides(id)))
        level_skip_set(g, source > 0 ? source : g->cutscene_default_level);
}
