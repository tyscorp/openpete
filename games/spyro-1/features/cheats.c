// games/spyro-1/features/cheats.c - --cheats: auto-apply Spyro 1's
// own built-in cheat codes.
//
// The game ships a pad-code cheat table (spyro-1/src/cheats.c). Matching a
// code calls CheatApplyEffect(), and two of its cases have PERSISTENT
// effects - plain writes into gameplay globals:
//
//   cheat 3, "LEVELS OPEN"  ->  g_VisitedFlags[0..31] = 1
//                               D_800758D0[0..5]      = 2  (homeworlds)
//   cheat 4, "99 LIVES"     ->  g_SpyroLifeCount      = 99
//
// --cheats replays exactly those writes so the code never has to be entered
// on the pad. The other table entries are deliberately not exposed: cheat 2
// fires the credits sequence (a one-shot action, not a persistent state) and
// cheats 0/1/5/6/7 have no CheatApplyEffect case at all in the retail build.
//
// Two ways in, one write path: --cheats picks the cheats a session starts
// with, and the dev overlay's Cheats rows carry a button per cheat that
// fires it on demand (re-clickable - "99 lives" tops the counter back up after
// dying). The overlay runs outside the sim, so a click only queues a request;
// every guest write happens in the per-tick hook below.
//
// TIMING. The globals live in BSS and are rewritten by whichever path starts
// a session - New Game resets progress/lives, SaveLoad fills them from the
// card - so a boot-time write would just be overwritten. Instead the cheats
// are applied on the first tick of live gameplay and re-armed whenever the
// title screen is showing, which is what makes them survive a quit-to-menu
// followed by New Game (each session gets them once, exactly as if the code
// had been entered by hand). One application per session is also what keeps
// "99 lives" a cheat rather than infinite lives: dying still costs one.
//
// Intentional, opt-in gameplay-state divergence - like the debug free camera,
// and unlike a presentation flag. OFF by default (zero writes), so a default
// run stays oracle-clean.
//
// Addresses from spyro-1/manifest/globals.toml; the effect mirrors
// CheatApplyEffect's own loop bounds (32 level flags, 6 homeworlds), not the
// declared array sizes.

#include "cheats.h"

#include "feature_registry.h"
#include "game_extras.h"
#include "openpete_fiber.h" /* openpete_speculative */
#include "openpete_knobs.h" /* dev-overlay rows (M key) */
#include "openpete_log.h"
#include "openpete_rewind.h"   /* openpete_rewind_note_intervention */
#include "openpete_surface.h"
#include "psx_runtime_internal.h" /* CPUState + its read/write callbacks */

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "openpete_thread_roles.h"

#define ADDR_G_GAMESTATE      0x800757D8u /* GS_Playing = 0, GS_TitleScreen = 13 */
#define ADDR_G_LEVELID        0x8007596Cu
#define ADDR_G_DEMOMODE       0x80075714u /* DEMO_MODE_NONE = 0 */
#define ADDR_G_SPYROLIFECOUNT 0x8007582Cu
#define ADDR_G_VISITEDFLAGS   0x80078E78u /* u8[36] */
#define ADDR_G_UNLOCKEDWORLDS 0x800758D0u /* u8[8]; D_800758D0 */

#define GS_PLAYING       0u
#define GS_PAUSEMENU     2u
#define GS_INVENTORYMENU 3u
#define GS_TITLESCREEN   13u

/** @brief Reports whether a game state is one where a cheat's writes make sense.
 *
 * Play, plus the pause and inventory screens. The inventory screen is where
 * the game's own pad codes are entered (CheatsProcess runs from gamestate 3),
 * so a cheat applied there is what the player could have typed.
 * @param gs The g_GameState value.
 * @return 1 for play, pause or inventory, else 0. */
static int gs_is_cheatable(uint32_t gs) {
    return gs == GS_PLAYING || gs == GS_PAUSEMENU || gs == GS_INVENTORYMENU;
}

#define CHEAT_99_LIVES         (1u << 0)
#define CHEAT_UNLOCK_ALL       (1u << 1)

typedef struct {
    const char* name;      /* --cheats spelling */
    uint32_t    bit;
    const char* help;      /* one-liner for --cheats errors + the boot log */
    const char* knob_id;   /* dev-overlay row id (stable slug) */
    const char* knob_label;/* dev-overlay row label */
    const char* knob_help; /* dev-overlay tooltip */
} cheat_def_t;

static const cheat_def_t k_cheats[] = {
    { "99_lives", CHEAT_99_LIVES,
      "99 extra lives (the \"99 LIVES\" code)",
      "cheat_99_lives", "99 lives",
      "Sets Spyro's extra lives to 99 - the game's own \"99 LIVES\" code. "
      "Applies on the next tick in a level; click again to top back up after "
      "dying." },
    { "unlock_all_levels", CHEAT_UNLOCK_ALL,
      "every level visited + every homeworld unlocked (the \"LEVELS OPEN\" code)",
      "cheat_unlock_all_levels", "Unlock all levels",
      "Marks every level visited and every homeworld unlocked - the game's own "
      "\"LEVELS OPEN\" code. Applies on the next tick in a level." },
};
#define CHEAT_COUNT (sizeof(k_cheats) / sizeof(k_cheats[0]))

/* --cheats selection (0 = none) and whether the current session still owes it
 * an application. Armed at startup so the first session gets it. */
static uint32_t s_mask  = 0;
static int      s_armed = 1;

/* Runtime (dev-overlay) state. The overlay runs outside the sim, so a button
 * press only sets a request bit here; the per-tick hook below does every guest
 * write at a tick boundary. Plain ints, like the other host-side toggles the
 * UI drives (freecam, fullscreen): each is a single word, and the worst a lost
 * race costs is applying one tick later.
 *   s_pending - cheats requested from the overlay, not yet written
 *   s_applied - cheats written during THIS session (status text; cleared when
 *               the title screen starts a new one)
 *   s_live    - cached "a level is up and it's the player's session", so the
 *               overlay can gray the button without reading guest RAM */
static uint32_t s_pending = 0;
static uint32_t s_applied = 0;
static int      s_live    = 0;

/** @brief Prints the valid --cheats names with their help lines to stderr. */
static void print_valid_names(void) {
    fprintf(stderr, "valid --cheats names:\n");
    for (size_t i = 0; i < CHEAT_COUNT; i++)
        fprintf(stderr, "  %-18s %s\n", k_cheats[i].name, k_cheats[i].help);
}

/** @brief Parses a comma-separated --cheats spec into a cheat mask.
 *
 * Names are matched case-insensitively with '-' accepted for '_'; blanks
 * around a name and empty names are skipped, and a name is truncated to 31
 * characters. An unknown name prints it and the valid list, since this runs
 * in CLI-parse context.
 * @param spec The spec.
 * @param out_mask Receives the mask on success.
 * @return 0 on success, -1 on an unknown name. */
static int parse_spec(const char* spec, uint32_t* out_mask) {
    uint32_t mask = 0;

    for (const char* p = spec; *p;) {
        char   norm[32];
        size_t n = 0;

        while (*p == ',' || isspace((unsigned char)*p)) p++;
        while (*p && *p != ',') {
            char c = (char)tolower((unsigned char)*p++);
            if (n < sizeof(norm) - 1) norm[n++] = (c == '-') ? '_' : c;
        }
        while (n > 0 && isspace((unsigned char)norm[n - 1])) n--;
        norm[n] = '\0';
        if (n == 0) continue;

        size_t i;
        for (i = 0; i < CHEAT_COUNT; i++) {
            if (strcmp(norm, k_cheats[i].name) == 0) { mask |= k_cheats[i].bit; break; }
        }
        if (i == CHEAT_COUNT) {
            fprintf(stderr, "openpete: --cheats: unknown cheat '%s'\n", norm);
            print_valid_names();
            return -1;
        }
    }

    *out_mask = mask;
    return 0;
}

int spyro1_cheats_validate(const char* spec) {
    uint32_t mask = 0;
    if (!spec || !*spec) {
        fprintf(stderr, "openpete: --cheats: expected a cheat name\n");
        print_valid_names();
        return -1;
    }
    if (parse_spec(spec, &mask) != 0) return -1;
    if (mask == 0) {
        fprintf(stderr, "openpete: --cheats: no cheat names in '%s'\n", spec);
        print_valid_names();
        return -1;
    }
    return 0;
}

/* ---- dev-overlay rows (one per cheat, fired by a button) -------------- */

/** @brief Finds the cheat a dev-overlay row fires, by the row's id.
 *
 * @param k The row.
 * @return The cheat's definition, or NULL when no cheat's knob_id matches. */
static const cheat_def_t* def_for_row(const knob_desc_t* k) {
    for (size_t i = 0; i < CHEAT_COUNT; i++)
        if (!strcmp(k->id, k_cheats[i].knob_id)) return &k_cheats[i];
    return NULL;
}

/** @brief Reports whether a cheat row's button is enabled: only while gameplay is live.
 *
 * @param k The row (unused).
 * @return 1.0 while gameplay is live, else 0.0. */
static double row_ready(const knob_desc_t* k) { (void)k; return s_live ? 1.0 : 0.0; }

/** @brief Queues a cheat row's cheat for the next tick.
 *
 * Sets the cheat's bit in s_pending; the tick hook makes the guest writes.
 * @param k The row.
 * @param v Unused.
 * @return 0, or -1 when the row matches no cheat. */
static int row_fire(knob_desc_t* k, double v) {
    const cheat_def_t* d = def_for_row(k);
    (void)v;
    if (!d) return -1;
    s_pending |= d->bit;   /* the sim tick does the writing */
    return 0;
}

/** @brief Fills a cheat row's status text: queued, not in a level, applied, or empty.
 *
 * Tested in that order: the cheat's pending bit, then gameplay not live, then
 * its applied bit. An unknown row gives empty text.
 * @param k The row.
 * @param buf Receives the text.
 * @param cap The capacity of buf. */
static void row_status(const knob_desc_t* k, char* buf, size_t cap) {
    const cheat_def_t* d = def_for_row(k);
    if (!d)                        snprintf(buf, cap, "%s", "");
    else if (s_pending & d->bit)   snprintf(buf, cap, "queued");
    else if (!s_live)              snprintf(buf, cap, "not in a level");
    else if (s_applied & d->bit)   snprintf(buf, cap, "applied");
    else                           snprintf(buf, cap, "%s", "");
}

/** @brief The dev-overlay row of cheat i without its value plumbing: a live KNOB_ACTION labelled "Apply" in the Cheats section.
 *
 * @param i Index into k_cheats.
 * @return The row template. */
static knob_desc_t cheat_row(size_t i) {
    knob_desc_t row = {0};
    row.id           = k_cheats[i].knob_id;
    row.label        = k_cheats[i].knob_label;
    row.section      = "Cheats";
    row.help         = k_cheats[i].knob_help;
    row.type         = KNOB_ACTION;
    row.mut          = KNOB_LIVE;
    row.action_label = "Apply";
    return row;
}

/** @brief Registers one dev-overlay action button per cheat in the Cheats section.
 *
 * Each row is cheat_row's, with row_ready as its enabled state, row_fire as
 * its action and row_status as its status text. A failed registration is
 * ignored. */
static void register_rows(void) {
    for (size_t i = 0; i < CHEAT_COUNT; i++) {
        knob_desc_t row = cheat_row(i);
        row.get_num = row_ready;
        row.set_num = row_fire;
        row.get_str = row_status;
        (void)openpete_knob_add(&row);
    }
}

void spyro1_cheats_surface(struct openpete_surface_sink* sink) {
    for (size_t i = 0; i < CHEAT_COUNT; i++) {
        knob_desc_t row = cheat_row(i);
        openpete_surface_row(sink, &row);
        openpete_surface_vocab(sink, "cheat", "--cheats", k_cheats[i].name);
    }
}

/** @brief Resets the cheat state, registers the overlay rows and arms the --cheats selection.
 *
 * Clears the mask, the pending and applied sets and arms the first session.
 * A NULL or empty opts->cheats, or a spec that parse_spec rejects (the mask
 * stays 0), arms nothing; each armed cheat is logged.
 * @param opts The options, or NULL. */
static void cheats_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    s_mask    = 0;
    s_armed   = 1;
    s_pending = 0;
    s_applied = 0;

    register_rows();

    if (!opts || !opts->cheats || !opts->cheats[0]) return;
    if (parse_spec(opts->cheats, &s_mask) != 0) { s_mask = 0; return; }

    for (size_t i = 0; i < CHEAT_COUNT; i++)
        if (s_mask & k_cheats[i].bit)
            OPL_INFO_F("cheats", "%s armed - %s",
                       k_cheats[i].name, k_cheats[i].help);
}

/** @brief Makes the guest writes of the cheats in a mask and logs each.
 *
 * 99_lives stores 99 to g_SpyroLifeCount. unlock_all_levels stores 1 to
 * g_VisitedFlags[0..31] and 2 to D_800758D0[0..5], CheatApplyEffect's loop
 * bounds.
 * @param cpu The guest CPU state.
 * @param mask The CHEAT_* bits to apply. */
static void cheats_apply(CPUState* cpu, uint32_t mask) {
    if (mask & CHEAT_99_LIVES) {
        cpu->write_word(ADDR_G_SPYROLIFECOUNT, 99u);
    }
    if (mask & CHEAT_UNLOCK_ALL) {
        for (uint32_t i = 0; i < 32u; i++)
            cpu->write_byte(ADDR_G_VISITEDFLAGS + i, 1u);
        for (uint32_t i = 0; i < 6u; i++)
            cpu->write_byte(ADDR_G_UNLOCKEDWORLDS + i, 2u);
    }
    for (size_t i = 0; i < CHEAT_COUNT; i++)
        if (mask & k_cheats[i].bit)
            OPL_INFO_F("cheats", "%s applied in level %u", k_cheats[i].name,
                       (unsigned)cpu->read_word(ADDR_G_LEVELID));
    if (mask) openpete_rewind_note_intervention("cheat");
}

void spyro1_cheats_on_tick(CPUState* cpu) {
    /* Runahead: a speculative tick's guest writes are rolled back, but the
     * latches here are host state that would not be - the cheat would be
     * dropped. Let the real tick do it. */
    if (openpete_speculative) return;

    /* Live = a level is up and it's the player's own session. The attract demo
     * runs GS_Playing with a level loaded, so it needs excluding explicitly;
     * it hands back to the title screen, which re-arms. */
    uint32_t gs = cpu->read_word(ADDR_G_GAMESTATE);
    s_live = (gs_is_cheatable(gs) &&
              cpu->read_word(ADDR_G_LEVELID) != 0u &&
              cpu->read_word(ADDR_G_DEMOMODE) == 0u);

    /* Back at the title screen: the next session (New Game or a loaded save)
     * starts from freshly reset globals, so owe it another pass. */
    if (gs == GS_TITLESCREEN) {
        s_armed   = 1;
        s_applied = 0;
        return;
    }
    if (!s_live) return;

    /* First live tick of a session: queue whatever --cheats asked for. From
     * here on both entry points share one route - the queue the overlay's
     * buttons write into, drained below. */
    if (s_armed) {
        s_armed    = 0;
        s_pending |= s_mask;
    }
    if (!s_pending) return;

    cheats_apply(cpu, s_pending);
    s_applied |= s_pending;
    s_pending  = 0;
}

const feature_module_t cheats_module = {
    .name           = "cheats",
    .apply_options  = cheats_apply_options,
    .dispatch_table = NULL,
};
