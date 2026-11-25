// games/spyro-3/skip_to_level.c - --skip-to-level for Spyro: Year of the Dragon.
//
// Loads directly into a level by inducing the title screen's own
// demo-timeout transition, seeded so its picker loads the target instead of
// the next attract demo, then holding isDemoMode clear until the level is
// playable so the controller drives Spyro and the level never ends as a demo
// would.
//
// The title overlay's state machine (title.ovl, SCUS-94467), updated once per
// frame by func_title_8007566C while game.state == GAMESTATE_TITLE_SCREEN:
//   g_TitleSubState  0x8007F12C  byte; 1 = press-start idle, 21 = demo
//                                timeout, 20 = New Game / Continue
//   g_TitlePrevState 0x8007F12D  byte; the update's tail compares it with
//                                the sub-state and zeroes the phase bytes
//                                below on a change
//   g_TitlePhase     0x8007F12E..0x8007F132  per-sub-state phase counters
//   g_TitleIdleTimer 0x8007F138  word; +1 per frame in sub-state 1, zeroed
//                                by any pad press; 900 triggers the timeout
//   g_DemoLevelList  0x80066EDC  bytes, 0xFF-terminated: 13 33 22 31 33 44 35
//   g_DemoAreaList   0x80066EE4  bytes, parallel: the sub-area each demo
//                                starts in
//   g_DemoIndex      0x8006C3F0  word, post-incremented by the picker, wraps
//                                at the 0xFF sentinel
//   currentLevel     0x8006C5BC  level number the loader loads (10..50)
//   area             0x8006C5C8  sub-area of the level
//   isDemoMode       0x8006C658  1 from the timeout until the level exits
//   game.state       0x8006E344  enum Gamestate: 0 gameplay, 3 fade-in,
//                                5 loading image, 11 title screen
//   loadStage        0x8006C518  loading overlay's state; -1 once the level
//                                overlay is resident
// Sub-state 1 with the idle timer at 900 sets isDemoMode = 1 and sub-state
// 21 (0x80075D38). Sub-state 21's handler (func_title_8007777C) waits for its
// fade, stops the title music (phase 0), then runs the picker
// (func_title_800777DC): currentLevel = list[g_DemoIndex], area =
// arealist[g_DemoIndex++], per-demo start position, WarpToLevel(0,
// currentLevel) (func_800584BC: game.state = 5, loadStage = 0), isDemoMode =
// 1, demo input buffer pointers. The pad path substitutes the recorded input
// while isDemoMode is set and the level exits when the recording runs out;
// the loading overlay also starts a demo in its sub-area (UpdateLoadingImage
// case 1) and takes the demo branch of its fade-in (case 3).

#include "skip_to_level.h"
#include "features/cutscene_skip.h"
#include "features/skip_to_area.h"
#include "features/level_skip.h"

#include <stdio.h>

#define S3_TITLE_SUBSTATE   0x8007F12Cu
#define S3_TITLE_PREVSTATE  0x8007F12Du
#define S3_TITLE_PHASE      0x8007F12Eu   /* 5 bytes through 0x8007F132 */
#define S3_DEMO_LEVEL_LIST  0x80066EDCu
#define S3_DEMO_AREA_LIST   0x80066EE4u
#define S3_DEMO_INDEX       0x8006C3F0u
#define S3_CURRENT_LEVEL    0x8006C5BCu
#define S3_IS_DEMO_MODE     0x8006C658u
#define S3_GAME_STATE       0x8006E344u
#define S3_LOAD_STAGE       0x8006C518u
#define S3_DEMO_ENDED       0x8006C7E4u   /* func_80050F18's demo-end flag */

#define S3_GS_CUTSCENE       6u
#define S3_SUBSTATE_IDLE     1u
#define S3_SUBSTATE_TIMEOUT  21u
#define S3_SUBSTATE_MENU_LO  2u
#define S3_SUBSTATE_MENU_HI  20u
/* Menu sub-states whose handlers drive the memory card (they call
 * func_title_8007CD68, func_title_800793C8 or func_title_8007928C): 6, 10,
 * 11, 16, 18. */
#define S3_CARD_SUBSTATES    ((1u << 6) | (1u << 10) | (1u << 11) | \
                              (1u << 16) | (1u << 18))
#define S3_GS_GAMEPLAY       0u
#define S3_GS_SPEECH         1u
#define S3_GS_FADE_IN        3u
#define S3_GS_LOADING_IMG    5u
#define S3_GS_TITLE          11u
#define S3_LOAD_RESIDENT     0xFFFFFFFFu

/* Level numbers are the game's own (currentLevel); names are the levels'
 * titles in lower_snake_case and displays those titles. */
static const game_level_t k_levels[] = {
    { "sunrise_spring",    "Sunrise Spring",    10 },
    { "sunny_villa",       "Sunny Villa",       11 },
    { "cloud_spires",      "Cloud Spires",      12 },
    { "molten_crater",     "Molten Crater",     13 },
    { "seashell_shore",    "Seashell Shore",    14 },
    { "mushroom_speedway", "Mushroom Speedway", 15 },
    { "sheilas_alp",       "Sheila's Alp",      16 },
    { "buzzs_dungeon",     "Buzz's Dungeon",    17 },
    { "crawdad_farm",      "Crawdad Farm",      18 },
    { "midday_gardens",    "Midday Gardens",    20 },
    { "icy_peak",          "Icy Peak",          21 },
    { "enchanted_towers",  "Enchanted Towers",  22 },
    { "spooky_swamp",      "Spooky Swamp",      23 },
    { "bamboo_terrace",    "Bamboo Terrace",    24 },
    { "country_speedway",  "Country Speedway",  25 },
    { "sgt_byrds_base",    "Sgt. Byrd's Base",  26 },
    { "spikes_arena",      "Spike's Arena",     27 },
    { "spider_town",       "Spider Town",       28 },
    { "evening_lake",      "Evening Lake",      30 },
    { "frozen_altars",     "Frozen Altars",     31 },
    { "lost_fleet",        "Lost Fleet",        32 },
    { "fireworks_factory", "Fireworks Factory", 33 },
    { "charmed_ridge",     "Charmed Ridge",     34 },
    { "honey_speedway",    "Honey Speedway",    35 },
    { "bentleys_outpost",  "Bentley's Outpost", 36 },
    { "scorchs_pit",       "Scorch's Pit",      37 },
    { "starfish_reef",     "Starfish Reef",     38 },
    { "midnight_mountain", "Midnight Mountain", 40 },
    { "crystal_islands",   "Crystal Islands",   41 },
    { "desert_ruins",      "Desert Ruins",      42 },
    { "haunted_tomb",      "Haunted Tomb",      43 },
    { "dino_mines",        "Dino Mines",        44 },
    { "harbor_speedway",   "Harbor Speedway",   45 },
    { "agent_9s_lab",      "Agent 9's Lab",     46 },
    { "sorceresss_lair",   "Sorceress's Lair",  47 },
    { "bugbot_factory",    "Bugbot Factory",    48 },
    { "super_bonus_round", "Super Bonus Round", 50 },
};
/* Resolution drops apostrophes, so "sheila's_alp" matches "sheilas_alp". */
const game_level_table_t spyro3_levels = {
    GAME_LEVEL_ROWS(k_levels), NULL, 0, "level", "--skip-to-level", 0u,
};

int spyro3_level_resolve(const char* arg) {
    return game_level_resolve(&spyro3_levels, arg);
}

int spyro3_skip_gameplay_live(CPUState* cpu) {
    return cpu->read_word(S3_LOAD_STAGE) == S3_LOAD_RESIDENT &&
           cpu->read_word(S3_GAME_STATE) == S3_GS_GAMEPLAY;
}

/** @brief Reports whether the picked level's entry sequence has begun: resident, and fading in, in a speech or in gameplay.
 *
 * The present release edge of --skip-to-level. Every level's entry runs
 * game.state 5 (loading image), 3 (fade-in from black), then 0, or 1 for a
 * level that opens on a first-visit speech, whose camera move and text box
 * play before it waits for a button. Releasing on the fade-in's first tick
 * presents the sequence a level transition shows after its loading image.
 * @param cpu The guest CPU state.
 * @return 1 when loadStage is -1 and game.state is 0, 1 or 3, else 0. */
static int skip_presentable(CPUState* cpu) {
    if (cpu->read_word(S3_LOAD_STAGE) != S3_LOAD_RESIDENT) return 0;
    const uint32_t gs = cpu->read_word(S3_GAME_STATE);
    return gs == S3_GS_GAMEPLAY || gs == S3_GS_SPEECH || gs == S3_GS_FADE_IN;
}

/* The demo level and area list bytes the induction overwrote, put back on
 * the pick. */
static uint32_t s_demo_slot;
static uint8_t  s_demo_slot_level, s_demo_slot_area;

/** @brief Reports whether the title's sub-state is an induction window.
 *
 * The press-start idle sub-state 1 at phase 1 (its first frames still fade
 * the menu in at phase 0, and the timeout fires from phase 1), or a menu
 * sub-state Press Start opens, 2 through 20, other than the five that drive
 * the memory card.
 * @param cpu The guest CPU state.
 * @return 1 in a window, else 0. */
static int induce_window(CPUState* cpu) {
    const uint32_t sub = cpu->read_byte(S3_TITLE_SUBSTATE);
    if (sub == S3_SUBSTATE_IDLE) return cpu->read_byte(S3_TITLE_PHASE) == 1u;
    return sub >= S3_SUBSTATE_MENU_LO && sub <= S3_SUBSTATE_MENU_HI &&
           !((S3_CARD_SUBSTATES >> sub) & 1u);
}

/** @brief Induces the title's idle timeout toward the target, without its isDemoMode = 1 store.
 *
 * Waits for the title (game.state 11) in an induction window
 * (induce_window). Writes the target into the demo
 * level list at g_DemoIndex with sub-area 0, then the timeout's own store
 * plus the update tail's change handling: sub-state 21 into g_TitleSubState
 * and g_TitlePrevState, and the five phase bytes zeroed.
 * @param cpu The guest CPU state.
 * @param target The level number.
 * @param log_vbl The VBL the log line prints.
 * @return 1 when induced, 0 while the window is not open. */
static int skip_induce(CPUState* cpu, int target, uint32_t log_vbl) {
    if (cpu->read_word(S3_GAME_STATE) != S3_GS_TITLE || !induce_window(cpu))
        return 0;
    uint32_t idx = cpu->read_word(S3_DEMO_INDEX);
    s_demo_slot       = idx;
    s_demo_slot_level = cpu->read_byte(S3_DEMO_LEVEL_LIST + idx);
    s_demo_slot_area  = cpu->read_byte(S3_DEMO_AREA_LIST + idx);
    cpu->write_byte(S3_DEMO_LEVEL_LIST + idx, (uint8_t)target);
    cpu->write_byte(S3_DEMO_AREA_LIST + idx, 0u);
    cpu->write_byte(S3_TITLE_SUBSTATE, S3_SUBSTATE_TIMEOUT);
    cpu->write_byte(S3_TITLE_PREVSTATE, S3_SUBSTATE_TIMEOUT);
    for (int i = 0; i < 5; i++) cpu->write_byte(S3_TITLE_PHASE + i, 0u);
    fprintf(stderr, "[skip-to-level] induced demo-timeout: level=%d "
                    "(slot=%u) at vbl=%u\n",
            target, (unsigned)idx, (unsigned)log_vbl);
    return 1;
}

/** @brief Tests whether the loader took the target: game.state 5 (loading image) with currentLevel equal to it.
 *
 * @param cpu The guest CPU state.
 * @param target The level number.
 * @return 1 when taken, else 0. */
static int skip_picked(CPUState* cpu, int target) {
    return cpu->read_word(S3_GAME_STATE) == S3_GS_LOADING_IMG &&
           cpu->read_word(S3_CURRENT_LEVEL) == (uint32_t)target;
}

/** @brief Puts back the demo level and area list bytes the induction overwrote.
 *
 * Runs on the pick. The picker func_title_800777DC loads both bytes
 * (0x80077814, 0x80077828) before its WarpToLevel call sets the game.state 5
 * the pick test reads; nothing else reads the lists. A later attract demo of
 * the slot then loads the slot's own level and sub-area.
 * @param cpu The guest CPU state. */
static void skip_restore(CPUState* cpu) {
    cpu->write_byte(S3_DEMO_LEVEL_LIST + s_demo_slot, s_demo_slot_level);
    cpu->write_byte(S3_DEMO_AREA_LIST + s_demo_slot, s_demo_slot_area);
    fprintf(stderr, "[skip-to-level] demo slot %u restored to level %u "
                    "area %u\n", (unsigned)s_demo_slot,
            (unsigned)s_demo_slot_level, (unsigned)s_demo_slot_area);
}

/** @brief Holds isDemoMode clear for one tick, from the pick to the playable edge.
 *
 * The pad path then reads the controller, the loader takes its non-demo
 * branches and the recording's end never exits the level. The flag's only
 * nonzero stores are the title update's (func_title_8007566C at 0x80075D50
 * and 0x800779F8), so it stays clear through the level once the hold ends
 * on the playable edge. game.state 11 before that edge means the level was
 * left.
 * @param cpu The guest CPU state.
 * @return 1 while the hold runs, 0 at game.state 11. */
static int skip_hold(CPUState* cpu) {
    if (cpu->read_word(S3_GAME_STATE) == S3_GS_TITLE) return 0;
    cpu->write_word(S3_IS_DEMO_MODE, 0u);
    return 1;
}

/* An armed --skip-to-area watch decides first, then a --skip-to-cutscene
 * watch; with neither, the first fade-in, speech or live tick after the
 * pick. The fail-safe deadline re-sets on each game.state 6 (cutscene)
 * entry, since a cutscene chain holds the game out of gameplay for longer
 * than one deadline. */
static const level_skip_watch_t k_watches[] = {
    { spyro3_area_watching, spyro3_area_released, spyro3_area_finish },
    { spyro3_cutscene_watching, spyro3_cutscene_released, spyro3_cutscene_finish },
};

const level_skip_game_t spyro3_level_skip = {
    .induce          = skip_induce,
    .picked          = skip_picked,
    .restore         = skip_restore,
    .hold            = skip_hold,
    .playable        = spyro3_skip_gameplay_live,
    .presentable     = skip_presentable,
    .watches         = k_watches,
    .n_watches       = sizeof k_watches / sizeof k_watches[0],
    .rearm_word      = S3_GAME_STATE,
    .rearm_value     = S3_GS_CUTSCENE,
};

void spyro3_skip_end_demo(CPUState* cpu) { cpu->write_word(S3_DEMO_ENDED, 1u); }

void spyro3_skip_set(int number) { level_skip_set(&spyro3_level_skip, number); }

int spyro3_skip_target(void) { return level_skip_target(&spyro3_level_skip); }

int spyro3_skip_level_picked(void) { return level_skip_picked(); }

void spyro3_skip_ffwd_arm(const char* until) { level_skip_ffwd_arm(until); }

void spyro3_skip_on_tick(CPUState* cpu, uint32_t frame_counter) {
    level_skip_on_tick(&spyro3_level_skip, cpu, frame_counter);
}
