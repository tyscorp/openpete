// games/spyro-1/game_extras_impl.c - Spyro 1's per-game hooks.
//
// Defines the runtime descriptor spyro1_game_runtime (openpete_game_runtime.h)
// over the hooks below and the tables of the game's other TUs. Identity is
// generated from recomp.conf (game_identity.c); this file holds the
// behavioural hooks: boot, the per-tick entries, savestate, the CLI
// resolvers and the per-function-entry overrides.
//
// Disc image: games/spyro-1/spyro-1/build/SCUS-94228.cue
// Disc BIN SHA256: fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37

#include "openpete_disc_build.h"
#include "game_extras.h"
#include "game_host_desc.h"
#include "launcher_assets.h"
#include "game_mod_desc.h"
#include "feature_registry.h"
#include "features/levelwarp_ui.h"
#include "openpete_launcher.h"
#include "openpete_mods.h"
#include "openpete_psx.h"
#include "openpete_ui_moby.h"
#include "openpete_fiber.h"
#include "openpete_skip.h"
#include "psx_runtime_internal.h"
#include "spyro_overlay_table.h"
#include "renderer/text_moby.h"
#include "features/audio_volume.h"
#include "features/vibration.h"
#include "features/cheats.h"
#include "features/credits_skip.h"
#include "features/level_skip.h"
#include "features/crash_demo_woah.h"
#include "freecam_game.h"
#include "write_census.h"
#include "dispatch_preamble.h"
#include "openpete_fillpick.h"
#include "features/levelwarp.h"
#include "features/state_watch.h"
#include "mods/mod_wad_overlay.h"
#include "psyq/psyq_thunk_state.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* mods/mod_level_ops.c, declared here: the TU has no header. */
void spyro1_mod_level_capture_stock_counts(CPUState* cpu);

/** @brief Snapshot the shipped level target tables, install the write census and register the resident-overlay probe, before any guest instruction runs.
 *
 * @param cpu Guest CPU state. */
static void spyro1_on_boot(CPUState* cpu) {
    /* The target gem/dragon/egg tables are EXE .data that the custom-level
     * registry overwrites per level; snapshot the shipped values while
     * they are still the only thing in those bytes. */
    spyro1_mod_level_capture_stock_counts(cpu);
    /* The write census installs over the store hooks before any guest
     * instruction runs; inert without OPENPETE_WRITE_CENSUS. */
    write_census_install(cpu);
    /* The mods dispatcher qualifies overlay-window overrides by the
     * resident overlay; no feature entry of this game carries an overlay
     * id, so the registry's own use of the probe changes nothing. */
    feature_registry_set_overlay_probe(game_resident_overlay_probe);
}

extern unsigned openpete_frame;

/* Terrain-reproject probe; documented at its declaration in psx_runtime_internal.h. */
void openpete_envvert_probe(void* cpu_v, uint32_t w0, uint32_t w1,
                            uint32_t s1, uint32_t s7, uint32_t t8,
                            uint32_t t9) {
    static FILE*    s_ev_fp = NULL;
    static int      s_ev_init = 0;
    static uint32_t s_ev_skip = 0, s_ev_cap = 80, s_ev_lastf = 0xFFFFFFFFu,
                    s_ev_frames = 0;
    if (!s_ev_init) {
        s_ev_init = 1;
        const char* p = getenv("OPENPETE_ENVVERT_LOG");
        if (p && *p) {
            const char* sk = getenv("OPENPETE_ENVVERT_LOG_SKIP");
            if (sk && *sk) s_ev_skip = (uint32_t)atoi(sk);
            const char* fl = getenv("OPENPETE_ENVVERT_LOG_FRAMES");
            if (fl && *fl) s_ev_cap = (uint32_t)atoi(fl);
            s_ev_fp = fopen(p, "wb");
            if (s_ev_fp) {
                uint32_t h[8] = { 0x54525645u /*"EVRT"*/, 1u, 48u,
                                  0,0,0,0,0 };
                fwrite(h, sizeof(h), 1, s_ev_fp);
            }
        }
    }
    if (!s_ev_fp) return;
    if (openpete_frame < s_ev_skip) return;
    if (openpete_frame != s_ev_lastf) {
        if (s_ev_frames >= s_ev_cap) {
            fflush(s_ev_fp); fclose(s_ev_fp); s_ev_fp = NULL; return;
        }
        s_ev_lastf = openpete_frame;
        s_ev_frames++;
    }
    CPUState* cpu = (CPUState*)cpu_v;
    uint8_t b[48];
    memcpy(b +  0, &openpete_frame, 4);
    memcpy(b +  4, &s1, 4);
    memcpy(b +  8, &w0, 4);
    memcpy(b + 12, &w1, 4);
    memcpy(b + 16, &s7, 4);
    memcpy(b + 20, &t8, 4);
    memcpy(b + 24, &t9, 4);
    for (int k = 0; k < 9; k++) {              /* g_Camera.m_ViewMatrix */
        int16_t v = (int16_t)cpu->read_half(0x80076DD0u + 0x14u + k*2u);
        memcpy(b + 28 + k * 2, &v, 2);
    }
    int16_t pad = 0; memcpy(b + 46, &pad, 2);
    fwrite(b, 1, 48, s_ev_fp);
}

/* --skip-to-level: load directly into a specific level instead of the
 * title-screen, Press-Start and save-file-picker flow, through the shared
 * driver (games/common/features/level_skip.c).
 *
 * The skip induces the transition the title's 990-tick demo timeout takes
 * (overlays/titlescreen.c:190), seeded so it loads the target. The game's
 * flow is:
 *   TSM_Init SubState 5 (timeout) -> m_Mode=TSM_Demo, m_State=TSS_Setup,
 *                                    m_DemoType=TSD_DemoLevel
 * TSS_Setup with TSD_DemoLevel (gamestates/update.c:636) then runs:
 *   func_8001277C();                     // Reset progress/lives/dragons
 *   levelId = g_DemoLevelIds[g_DemoIndex];
 *   g_LevelId = levelId; g_NextLevelId = levelId;
 * Followed by TSS_Loading -> LoadLevel(1) loop and TSS_Active flyby.
 *
 * The skip (a) overwrites g_DemoLevelIds[g_DemoIndex] with the target and
 * (b) forces the TSM_Init->TSM_Demo transition the timeout path takes. The
 * game's state machine then runs the state reset, level load and flyby
 * intro as it does for the demo. (TSD_DemoLevel sets g_DemoMode = PLAY at
 * tick 384 of the flyby; that is the only difference between the demo and
 * play, since the same TSM_Demo, LoadLevel and flyby sequence runs when a
 * player picks New Game.) TSD_DemoLevel, unlike TSD_Level, runs
 * func_8001277C's progress/lives/health reset, which a freshly booted
 * TSD_Level skip would miss.
 *
 * Hook point: TSM_Init SubState 3 (Press-Start wait, after the title's own
 * load and intro animation finish). Earlier races the CD load of the title
 * data; at SubState 4/5 the player already pressed Start or the timeout
 * already fired.
 *
 * The menus Press Start opens (TSM_Menu, the card screens, and TSM_Loading,
 * the save-file picker) have no way back to SubState 3, so there the skip
 * makes the picker's own commit instead (overlays/titlescreen.c:1027-1044):
 * m_Mode TSM_Loading with m_State 5 fades out, calls MemCardStop and enters
 * TSM_Demo at TSS_Setup with the m_DemoType it was given. It does so from a
 * state waiting on the pad (m_SubTick >= 8), outside the TSM_Menu sub-states
 * that read, format or write a card (0, 1, 7, 11).
 *
 * Addresses (spyro-1/manifest/globals.toml):
 *   g_Gamestate         = 0x800757D8  (GS_TitleScreen = 13,
 *                                      GS_Playing = 0)
 *   g_DemoMode          = 0x80075714  (clamp to 0 each tick)
 *   g_DemoLevelIds      = 0x8006EE7C  (int[] in .data)
 *   g_DemoIndex         = 0x800757B8
 *   g_TitlescreenState  = 0x80078D78
 *     +0x00 m_Mode      (TSM_Init=0, TSM_Demo=3)
 *     +0x04 m_State     (TSS_Setup=0)
 *     +0x10 m_SubState  (TSM_Init's =3 means "Press Start" wait)
 *     +0x1C m_DemoType  (TSD_DemoLevel=2) */
#define S1_GAMESTATE   0x800757D8u
#define S1_DEMO_MODE   0x80075714u
#define S1_DEMO_LEVELS 0x8006EE7Cu
#define S1_DEMO_INDEX  0x800757B8u
#define S1_TSM         0x80078D78u

#define S1_TSM_INIT    0u
#define S1_TSM_MENU    1u
#define S1_TSM_LOADING 2u
#define S1_TSM_DEMO    3u
#define S1_TSD_DEMOLEVEL 2u
/* TSM_Menu sub-states with a card read, format or write in flight. */
#define S1_MENU_CARD_SUBSTATES ((1u << 0) | (1u << 1) | (1u << 7) | (1u << 11))

/* The g_DemoLevelIds entry the induction overwrote, put back on the pick. */
static uint32_t s_demo_slot, s_demo_slot_level;

/** @brief Reports whether a Press Start menu waits on the pad outside a card access, so its commit can be made.
 *
 * TSM_Menu (m_SubState at +0x10, other than the card sub-states) and
 * TSM_Loading's pickers (m_State 1-4 at +0x04), each with m_SubTick
 * (+0x0C) at 8 or more, the threshold their pad tests use.
 * @param cpu The guest CPU state.
 * @param mode m_Mode.
 * @return 1 in such a state, else 0. */
static int s1_menu_waits(CPUState* cpu, uint32_t mode) {
    if (cpu->read_word(S1_TSM + 0x0Cu) < 8u) return 0;
    if (mode == S1_TSM_MENU) {
        const uint32_t sub = cpu->read_word(S1_TSM + 0x10u);
        return sub < 32u && !((S1_MENU_CARD_SUBSTATES >> sub) & 1u);
    }
    if (mode != S1_TSM_LOADING) return 0;
    const uint32_t st = cpu->read_word(S1_TSM + 0x04u);
    return st >= 1u && st <= 4u;
}

/** @brief Induces the title's demo-timeout transition toward the target at the Press-Start wait.
 *
 * Refuses a target whose guest WAD TOC entry is empty (data length 0):
 * loading one chases garbage and crashes. The free slot (id 65) only gains
 * an entry once a custom level's warp claims it, so it cannot be skip
 * targeted cold; OPENPETE_CUSTOM_WARP covers that.
 * @param cpu The guest CPU state.
 * @param target The level id.
 * @param log_vbl The VBL the log lines print.
 * From a Press Start menu waiting on the pad (s1_menu_waits) it makes the
 * picker's commit instead: m_Mode TSM_Loading, m_State 5, m_DemoType
 * TSD_DemoLevel and m_SubTick 0.
 * @return 1 when induced, 0 while neither at TSM_Init sub-state 3 nor in a
 *         waiting menu on the title screen, -1 when refused. */
static int s1_skip_induce(CPUState* cpu, int target, uint32_t log_vbl) {
    if (cpu->read_word(S1_GAMESTATE) != 13u) return 0;
    const uint32_t mode = cpu->read_word(S1_TSM + 0x00u);
    const int from_menu = s1_menu_waits(cpu, mode);
    if (!from_menu &&
        (mode != S1_TSM_INIT || cpu->read_word(S1_TSM + 0x10u) != 3u))
        return 0;
    int ti = (target / 10 - 1) * 6 + target % 10;
    if (ti < 0 || ti > 35 ||
        !cpu->read_word(0x8007A6D0u + 72u + 16u * (uint32_t)ti + 12u)) {
        fprintf(stderr,
                "[skip-to-level] level=%d has no WAD TOC data "
                "(unclaimed slot?) - refusing; for a custom "
                "level use OPENPETE_CUSTOM_WARP\n", target);
        return -1;
    }
    uint32_t demo_idx = cpu->read_word(S1_DEMO_INDEX);
    s_demo_slot       = demo_idx;
    s_demo_slot_level = cpu->read_word(S1_DEMO_LEVELS + demo_idx * 4u);
    cpu->write_word(S1_DEMO_LEVELS + demo_idx * 4u, (uint32_t)target);
    if (from_menu) {
        cpu->write_word(S1_TSM + 0x00u, S1_TSM_LOADING);
        cpu->write_word(S1_TSM + 0x04u, 5u); /* m_State: fade to TSM_Demo */
        cpu->write_word(S1_TSM + 0x0Cu, 0u); /* m_SubTick */
    } else {
        cpu->write_word(S1_TSM + 0x00u, S1_TSM_DEMO);
        cpu->write_word(S1_TSM + 0x04u, 0u); /* m_State = TSS_Setup */
    }
    cpu->write_word(S1_TSM + 0x1Cu, S1_TSD_DEMOLEVEL);
    fprintf(stderr,
            "[skip-to-level] induced %s: level=%d (slot=%u) at vbl=%u\n",
            from_menu ? "menu commit" : "demo-transition",
            target, (unsigned)demo_idx, (unsigned)log_vbl);
    return 1;
}

/** @brief Holds the title screen's demo state off the demo-input path, every tick from the induction to the playable edge.
 *
 * After TSS_Setup runs, m_DemoType goes from TSD_DemoLevel to TSD_Level.
 * TSS_Setup uses TSD_DemoLevel to call func_8001277C() (the progress and
 * lives reset), but the tick-384 hand-off (gamestates/update.c:770) gates
 * `g_DemoMode = DEMO_MODE_PLAY` on TSD_DemoLevel, and leaving it set makes
 * LoadLevel's stage-13 pass run the demo-input playback path
 * (loaders.c:672). g_DemoMode is also clamped to 0 while the title-screen
 * overlay is live, so no other path can set PLAY. The shared driver ends the
 * hold at GS_Playing, which LoadLevel's stage 13 sets after the hand-off;
 * g_DemoMode's only PLAY store is that hand-off, under GS_TitleScreen, so a
 * later return to the title runs the attract demo as the game does.
 * @param cpu The guest CPU state.
 * @return 1. */
static int s1_skip_hold(CPUState* cpu) {
    uint32_t gs     = cpu->read_word(S1_GAMESTATE);
    uint32_t tsm_st = cpu->read_word(S1_TSM + 0x04u);
    if (gs == 13u /*GS_TitleScreen*/) {
        if (tsm_st != 0u /* past TSS_Setup */)
            cpu->write_word(S1_TSM + 0x1Cu, 1u); /* TSD_Level */
        cpu->write_word(S1_DEMO_MODE, 0u); /* DEMO_MODE_NONE */
    }
    return 1;
}

/** @brief Tests whether TSS_Setup has taken the target: m_Mode TSM_Demo with m_State past TSS_Setup.
 *
 * TSS_Setup reads g_DemoLevelIds[g_DemoIndex] (gamestates/update.c:640) and
 * sets m_State to TSS_Loading in the same call. The mode test keeps a menu
 * commit's fade (TSM_Loading, m_State 5) from reading as the pick.
 * @param cpu The guest CPU state.
 * @param target Unused.
 * @return 1 once m_Mode is TSM_Demo and m_State is not TSS_Setup, else 0. */
static int s1_skip_picked(CPUState* cpu, int target) {
    (void)target;
    return cpu->read_word(S1_TSM + 0x00u) == S1_TSM_DEMO &&
           cpu->read_word(S1_TSM + 0x04u) != 0u;
}

/** @brief Puts back the g_DemoLevelIds entry the induction overwrote.
 *
 * Runs on the pick. TSS_Setup is the entry's only reader, so a later attract
 * demo of the slot loads the slot's own level for its recording.
 * @param cpu The guest CPU state. */
static void s1_skip_restore(CPUState* cpu) {
    cpu->write_word(S1_DEMO_LEVELS + s_demo_slot * 4u, s_demo_slot_level);
    fprintf(stderr, "[skip-to-level] demo slot %u restored to level %u\n",
            (unsigned)s_demo_slot, (unsigned)s_demo_slot_level);
}

/** @brief Tests the level skip's playable edge: g_Gamestate GS_Playing.
 *
 * TSS_Active is not the edge: the homeworld flyby after it is a title card
 * over black for about 380 VBLs.
 * @param cpu The guest CPU state.
 * @return 1 at GS_Playing, else 0. */
static int s1_skip_playable(CPUState* cpu) {
    return cpu->read_word(S1_GAMESTATE) == 0u;
}

static const level_skip_watch_t k_skip_watches[] = {
    { spyro1_cutscene_skip_watching, spyro1_cutscene_skip_released,
      spyro1_cutscene_skip_finish },
};

/* The pick is TSS_Setup's run, after which the hold starts. `intro`
 * runs no level skip; the credits rows, which name no source, ride
 * Artisans (10). */
static const level_skip_game_t k_level_skip = {
    .induce                 = s1_skip_induce,
    .picked                 = s1_skip_picked,
    .restore                = s1_skip_restore,
    .hold                   = s1_skip_hold,
    .playable               = s1_skip_playable,
    .watches                = k_skip_watches,
    .n_watches              = sizeof k_skip_watches / sizeof k_skip_watches[0],
    .cutscene_source        = spyro1_cutscene_source_level,
    .cutscene_arm           = spyro1_cutscene_skip_arm,
    .cutscene_default_level = 10,
};

/** @brief Arm a skip into a level through the title screen and fast-forward presents to it in a windowed run.
 *
 * The dev levelwarp's title route and the custom-level menu both come
 * through here.
 * @param level_id The target level id; 0 disables the skip. */
static void spyro1_set_skip_to_level(int level_id) {
    level_skip_set(&k_level_skip, level_id);
}

/** @brief Arm a --skip-to-cutscene target from features/credits_skip.c; `intro` runs no level skip and the credits default to Artisans (10).
 *
 * @param cutscene_id The cutscene id; 0 or less disables it. */
static void spyro1_set_skip_to_cutscene(int cutscene_id) {
    level_skip_arm_cutscene(&k_level_skip, cutscene_id);
}

/** @brief Report Spyro 1's cheat and level-warp rows and its cheat, level and cutscene vocabularies to the surface dump.
 *
 * @param sink The dump's collector. */
static void spyro1_surface(struct openpete_surface_sink* sink) {
    spyro1_cheats_surface(sink);
    levelwarp_surface(sink, levelwarp_level_rows);
    game_level_surface(&spyro1_levels, sink);
    spyro1_cutscene_surface(sink);
}

/** @brief Run Spyro 1's per-tick host hooks before the game fiber runs a tick: the HUD range, the scripted player pose and free camera, the features, the level skip and its release.
 *
 * @param cpu Guest CPU state.
 * @param frame_counter The openpete_frame counter before the tick. */
static void spyro1_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
    (void)frame_counter;

    static int s_ui_range_done = 0;
    if (!s_ui_range_done) {
        s_ui_range_done = 1;
        /* Register the static UI-moby range. g_Hud.m_Mobys[12] lives
         * at PSX vaddr 0x80077FEC (g_Hud at 0x80077FA8 + offset 0x44),
         * sizeof(Moby)=0x58, so the range is
         *   [0x80077FEC, 0x80077FEC + 12*0x58 = 0x8007840C).
         * Used by the native renderer's is_addr_ui/is_addr_static
         * HUD classification (moby.c). */
        openpete_ui_moby_set_static_range(0x80077FECu,
                                          0x80077FECu + 12u * 0x58u);
    }

    /* Scripted player placement (--player-pose) on the first placeable tick,
     * then the optional scripted engage of the debug free camera
     * (OPENPETE_FREECAM=1) once gameplay is live; placement first so one run
     * can view the placed player. Interactive F / stick-click use is
     * independent. */
    freecam_player_pose_maybe_apply(cpu);
    freecam_maybe_autoengage(cpu);

    /* The fill-tag inspector engages on the same "level is live" gate - a
     * scripted pick run sets OPENPETE_FILLPICK=1 alongside
     * OPENPETE_FREECAM_POSE and reads the [fillpick] lines off stderr. */
    {
        static int s_fp_done = 0;
        if (!s_fp_done && (int32_t)cpu->read_word(0x80075864u) < 0) {   /* g_LoadStage */
            s_fp_done = 1;
            const char* e = getenv("OPENPETE_FILLPICK");
            if (e && *e && *e != '0') openpete_fillpick_set_active(1);
            e = getenv("OPENPETE_FILLPICK_PREVIEW");
            if (e && *e && *e != '0') openpete_fillpick_set_preview(1);
        }
    }

    /* --cheats: apply the game's own built-in cheat effects once a level is
     * live, re-armed at the title screen so each new session gets them. Inert
     * without the flag. */
    spyro1_cheats_on_tick(cpu);

    /* Title-screen Crash-demo easter egg: pre-decode the woah while the
     * title screen is up, and re-arm between presses. Inert elsewhere. */
    spyro1_crash_woah_on_tick(cpu);

    /* --sfx / --music: hold the game's own volume sliders at the requested
     * levels (re-stamped over the title screen's and a save load's reseeds,
     * released when the player moves that slider), and mirror the music
     * slider onto the XA / music-WAV gain so it acts as a level rather than
     * an on/off switch. Inert without the flags apart from that mirror. */
    spyro1_audio_volume_on_tick(cpu);

    /* --vibration: hold the game's own vibration setting at the requested
     * state (re-stamped over the title screen's and a save load's reseeds,
     * released when the player toggles that row by hand). Inert without the
     * flag; the host half - motor strength, and the headless / focus gates -
     * lives in core/input/host_rumble.c. */
    spyro1_vibration_on_tick(cpu);

    /* Dev-overlay level warp (M menu): drives the game's own exit-level ->
     * load sequence toward a chosen level. Inert until the Warp button is
     * clicked. */
    spyro1_levelwarp_on_tick(cpu);


    /* The host text layers (shimmer, performance readout, settings menu, OSD
     * toasts, version line), built from Spyro's own 3D HUD glyph models read
     * from the disc. Pure presentation: reads and writes no guest state. */
    text_moby_native_layers_tick();

    /* --skip-to-level: induce the title's demo-timeout transition toward
     * the target, hold the title off the demo path, and release the present
     * fast-forward on the playable edge. Inert without the flag. */
    level_skip_on_tick(&k_level_skip, cpu, frame_counter);

    /* OPENPETE_STATE_WATCH: one stderr line per change of the named guest
     * words, after the skip and warp hooks as in the shared pre-tick hook.
     * Reads only; inert without the variable. */
    state_watch_on_tick(cpu, frame_counter);
}

/* Each overlay's first word at 0x8007AA38 is its id (LEVEL_BALLOON burns LONG(id) at +0x00). */
extern void spyro_set_active_overlay_by_magic(uint32_t magic);

/** @brief Re-point the active-overlay dispatcher after a savestate restore at the overlay whose id word 0x8007AA38 holds.
 *
 * @param cpu Guest CPU state. */
static void spyro1_on_state_loaded(CPUState* cpu) {
    uint32_t magic = cpu->read_word(0x8007AA38u);
    spyro_set_active_overlay_by_magic(magic);

}

/** @brief Report gameplay live: g_Gamestate GS_Playing and a nonzero g_LevelId, since GS_Playing is 0, which boot's BSS also reads.
 *
 * @param cpu Guest CPU state. */
static int spyro1_is_gameplay_live(CPUState* cpu) {
    if (cpu->read_word(0x800757D8u) != 0u) return 0; /* g_Gamestate != GS_Playing */
    return cpu->read_word(0x8007596Cu) != 0u;        /* g_LevelId */
}

/* Game-side extra savestate payload. Host-side state that mirrors or
 * gates guest execution and must travel with every snapshot - including
 * the per-tick runahead/rewind mem snapshots, whose speculative ticks can
 * run load/transition code (CdlSetloc moves the WAD cursor, an overlay
 * CdRead flips the dispatcher) that the rollback must undo. Bump the
 * version word when fields are added; core treats the blob as opaque, so
 * the layout is defined here.
 *   v1: WAD-overlay disc cursor (mods/mod_wad_overlay.c).
 *   v2: + active-overlay magic - restoring by saved id instead of the RAM
 *       marker word survives cutscene-style loads that clobber the marker.
 *   v3: + guest-mutated thunk latches (CD-ready / VSync callback vaddrs,
 *       XA arm + filter): CdlSetmode flips the XA route and load code
 *       re-registers callbacks, so a rolled-back tick that ran transition
 *       code otherwise leaves them on the abandoned branch.
 *   v4: + dev-overlay level-warp FSM (features/levelwarp.c). The
 *       phase gates guest writes across several ticks, so a rewind past a
 *       half-applied warp has to restore it alongside the RAM it wrote.
 *   v5: the thunk latches are the shared layer's pack
 *       (games/common/psyq/psyq_thunk_state.h: callbacks, drive mode and filter,
 *       the interrupt- and DMA-callback tables) in place of v3's three
 *       words; the warp words follow it at the length the pack's own
 *       version word gives, so a blob saved under an earlier pack version
 *       still loads. v3/v4 blobs load through the pack's legacy adapter. */
extern uint32_t spyro1_mod_wad_cursor_get(void);
extern void     spyro1_mod_wad_cursor_set(uint32_t s);

#define EXTRA_V5_PACK_OFF 12u
#define EXTRA_V5_WARP_OFF (EXTRA_V5_PACK_OFF + PSYQ_THUNK_STATE_BYTES)
#define EXTRA_V5_SIZE     (EXTRA_V5_WARP_OFF + 12u)

/** @brief Return the v5 payload's byte size. */
static uint32_t spyro1_state_extra_size(void) { return EXTRA_V5_SIZE; }

/** @brief Write the v5 payload: version, WAD cursor, active overlay, thunk latch pack and level-warp words.
 *
 * @param dst Receives EXTRA_V5_SIZE bytes. */
static void spyro1_state_extra_save(void* dst) {
    uint8_t* p = (uint8_t*)dst;
    uint32_t ver = 5u, cur = spyro1_mod_wad_cursor_get();
    uint32_t ovl = spyro_get_active_overlay_magic();
    uint32_t wphase, wtarget, wticks;
    spyro1_levelwarp_state_save(&wphase, &wtarget, &wticks);
    memcpy(p +  0, &ver, 4);
    memcpy(p +  4, &cur, 4);
    memcpy(p +  8, &ovl, 4);
    psyq_thunk_state_save(p + EXTRA_V5_PACK_OFF);
    memcpy(p + EXTRA_V5_WARP_OFF + 0, &wphase, 4);
    memcpy(p + EXTRA_V5_WARP_OFF + 4, &wtarget, 4);
    memcpy(p + EXTRA_V5_WARP_OFF + 8, &wticks, 4);
}

/** @brief Restore the host state from a v1 to v5 payload, leaving it alone for a foreign or short one.
 *
 * @param src The payload.
 * @param size The payload's length in bytes. */
static void spyro1_state_extra_load(const void* src, uint32_t size) {
    if (size < 8u) return;   /* older/foreign blob - leave live state alone */
    const uint8_t* p = (const uint8_t*)src;
    uint32_t ver, cur;
    memcpy(&ver, p + 0, 4);
    memcpy(&cur, p + 4, 4);
    if (ver >= 1u) spyro1_mod_wad_cursor_set(cur);
    if (ver >= 2u && size >= 12u) {
        uint32_t ovl;
        memcpy(&ovl, p + 8, 4);
        if (ovl) spyro_set_active_overlay_by_magic(ovl);
    }
    if (ver >= 5u) {
        uint32_t pack_ver, pack_len;
        if (size < EXTRA_V5_PACK_OFF + 4u) return;
        memcpy(&pack_ver, p + EXTRA_V5_PACK_OFF, 4);
        pack_len = psyq_thunk_state_bytes_for(pack_ver);
        if (pack_len == 0u || size < EXTRA_V5_PACK_OFF + pack_len + 12u) return;
        (void)psyq_thunk_state_load(p + EXTRA_V5_PACK_OFF, pack_len);
        uint32_t wphase, wtarget, wticks;
        const uint8_t* w = p + EXTRA_V5_PACK_OFF + pack_len;
        memcpy(&wphase,  w + 0, 4);
        memcpy(&wtarget, w + 4, 4);
        memcpy(&wticks,  w + 8, 4);
        spyro1_levelwarp_state_load(wphase, wtarget, wticks);
        return;
    }
    if (ver >= 3u && size >= 24u) {
        uint32_t cd_cb, vs_cb, xa;
        memcpy(&cd_cb, p + 12, 4);
        memcpy(&vs_cb, p + 16, 4);
        memcpy(&xa,    p + 20, 4);
        psyq_thunk_state_load_v4(cd_cb, vs_cb, xa);
    }
    if (ver >= 4u && size >= 36u) {
        uint32_t wphase, wtarget, wticks;
        memcpy(&wphase,  p + 24, 4);
        memcpy(&wtarget, p + 28, 4);
        memcpy(&wticks,  p + 32, 4);
        spyro1_levelwarp_state_load(wphase, wtarget, wticks);
    }
}

/* --- Disc layout --- */

static const game_disc_layout_t k_disc_layout = {
    .wad_sector      = 37,            /* Spyro 1 stores WAD.WAD at sector 37 */
    .wad_header_addr = 0x80100000u,   /* unread; the guest header is g_WadHeader at 0x8007A6D0 */
};

/* Recomp self-instrumentation. On entry to func_80023AC4 (Spyro's renderer, hand-written asm), wrap
 * cpu->write_word with a frame-gated tracer to capture the prim-buffer
 * write stream and diff bad vs good frames. The depth flag prevents the
 * recursive call into the function body from re-entering this branch. */
extern void func_80023AC4(CPUState* cpu);
extern void openpete_trace_writes_begin(CPUState* cpu);
extern void openpete_trace_writes_end(CPUState* cpu);

/** @brief Reports whether OPENPETE_NOOP_SREND suppresses the handwritten renderer at a guest address.
 *
 * The mask is latched on first use, since this sits on the per-call dispatch
 * path and getenv is expensive on Windows; a nonzero mask logs one warning
 * line. The bits are listed at the OPENPETE_NOOP_SREND arm in
 * spyro1_override_dispatch.
 * @param addr The guest function address being entered.
 * @return 1 when the mask suppresses that renderer, else 0. */
static int srend_noop_hit(uint32_t addr) {
    static int s_mask = -1;
    if (s_mask < 0) {
        const char* e = getenv("OPENPETE_NOOP_SREND");
        s_mask = e ? (int)strtol(e, NULL, 0) : 0;
        if (s_mask) {
            fprintf(stderr, "[noop-srend] mask=%d - renderers suppressed; "
                            "geometry and gameplay state are NOT faithful\n", s_mask);
        }
    }
    if (s_mask == 0) return 0;
    switch (addr) {
    case 0x800258F0u:                        /* world/terrain renderer      */
        return (s_mask & 1) != 0;
    case 0x8001F798u:                        /* regular material renderer   */
        /* Bits 16/32 take the two material EMIT passes separately: they build
         * prims and write no gameplay-readable byte (the cull passes own
         * m_WasDrawn), so suppressing them leaves every verdict in place -
         * the "stub the moby renderer" ceiling. Bit 2 additionally takes the
         * cull passes, which the game cannot survive. */
        if (s_mask & 16) return 1;
        return (s_mask & 2) != 0;
    case 0x80020F34u:                        /* specular material renderer  */
        if (s_mask & 32) return 1;
        return (s_mask & 2) != 0;
    case 0x8001F158u: case 0x800208FCu:      /* moby cull/gather passes     */
    case 0x80022A2Cu:                        /* shaded cull + renderer      */
        return (s_mask & 2) != 0;
    case 0x8004EBA8u:                        /* cyclorama dome              */
    case 0x80050BD0u:                        /* portal previews             */
        return (s_mask & 4) != 0;
    case 0x80023AC4u:                        /* Spyro                       */
    case 0x80058D64u:                        /* flame breath                */
    case 0x80059F8Cu: case 0x80059A48u:      /* moby + Spyro shadows        */
    case 0x800573C8u: case 0x800580F4u:      /* particles, glows            */
        return (s_mask & 8) != 0;
    default:
        return 0;
    }
}

/** @brief Runs Spyro 1's per-function-entry overrides in priority order and reports whether one handled the call.
 *
 * Called from the dispatcher on every translated-function entry, after the
 * PSX-trace hooks. Order: the shared preamble (game_dispatch_preamble:
 * debug free-camera freeze, write-census entry note, mod override chains),
 * the feature registry, OPENPETE_NOOP_SREND renderer suppression, then two
 * diagnostics: the OPENPETE_GEM_TRACE gem-cull dump at func_80022A2C
 * (openpete_frame 2999 and 3000, falls through to the original), and a
 * write-trace bracket around every entry to func_80023AC4 (Spyro's
 * renderer), which runs the function itself.
 * @param cpu Guest CPU state.
 * @param addr Guest address of the function being entered.
 * @return 1 when the call was handled or replaced and the original must not
 *     run, else 0. */
static int spyro1_override_dispatch(CPUState* cpu, uint32_t addr) {
    /* Shared preamble: the debug free camera (F), which while engaged
     * drives the camera in place of CameraFollowSpyro and suppresses it and
     * UpdateSpyro; the write census's entry note; the mod override chains.
     * Freecam (a user debug tool) wins over a mod, so freezing behaves
     * identically with mods loaded; a mod that replaces a function replaces
     * it ahead of the feature arm. */
    if (game_dispatch_preamble(cpu, addr, NULL)) {
        return 1;
    }

    /* Feature modules registered via core/include/feature_registry.h;
     * a game with no feature list makes this a no-op. */
    if (feature_registry_dispatch(cpu, addr)) {
        return 1;
    }

    /* Perf attribution knob (OPENPETE_NOOP_SREND=<mask>): turn the handwritten
     * PSX renderers into no-ops so the sim tick can be measured without the
     * emulated-GTE prim building whose output native mode discards. Placed
     * AFTER the feature arms on purpose: a native feeder that hooks a renderer
     * runs its own capture/feed and then calls the real renderer directly; that
     * inner call re-enters here (the feeder's re-entrancy guard has already
     * declined) and is the call this suppresses. So the feeds keep running and
     * only the guest-side prim math goes away.
     *
     * MEASUREMENT ONLY - the renderers also write gameplay-readable state
     * (sector visibility, m_WasDrawn, the moby render queue), so a run with any
     * bit set is not gameplay-faithful and its geometry will thin out.
     *   1  world/terrain (func_800258F0)
     *   2  mobys (2 cull/gather passes + 3 material renderers)
     *   4  cyclorama dome + portal previews
     *   8  Spyro, flame, shadows, particles, glows
     *  16  regular moby prim build ONLY (verdicts left intact)
     *  32  specular moby prim build ONLY (verdicts left intact)
     */
    if (srend_noop_hit(addr)) {
        return 1;
    }

    /* Widescreen and FOV are native-renderer display policies
     * (core/renderer/native_render.c); no dispatch arm here alters the
     * guest render path for them. */
    /* Diagnostic: trace bucket-2 (gems) cull. Walks D_8006FCF4+0x2400 list,
     * computes per-moby cube + MVMVA cone test the same way the inline
     * func_80022A2C cull does, prints which gems pass / fail. Used only
     * with OPENPETE_GEM_TRACE=1 to debug widescreen gem visibility. */
    static int s_in_a2c = 0;
    if (addr == 0x80022A2Cu && !s_in_a2c
        && getenv("OPENPETE_GEM_TRACE")
        && (openpete_frame == 3000u || openpete_frame == 2999u)) {
        s_in_a2c = 1;
        const uint32_t D = 0x8006FCF4u;
        const uint32_t pm = 0x80076DD0u;
        int32_t cam_x = (int32_t)cpu->read_word(pm + 0x28u);
        int32_t cam_y = (int32_t)cpu->read_word(pm + 0x2Cu);
        int32_t cam_z = (int32_t)cpu->read_word(pm + 0x30u);
        int16_t R11 = (int16_t)cpu->read_half(pm + 0x0u);
        int16_t R12 = (int16_t)cpu->read_half(pm + 0x2u);
        int16_t R13 = (int16_t)cpu->read_half(pm + 0x4u);
        int16_t R31 = (int16_t)cpu->read_half(pm + 0xCu);
        int16_t R32 = (int16_t)cpu->read_half(pm + 0xEu);
        int16_t R33 = (int16_t)cpu->read_half(pm + 0x10u);
        fprintf(stderr, "[gem-trace f=%u cam=(%d,%d,%d) R0=(%d,%d,%d) R2=(%d,%d,%d)]\n",
                openpete_frame, cam_x, cam_y, cam_z, R11, R12, R13, R31, R32, R33);
        for (int i = 0; i < 96; i++) {
            uint32_t mp = cpu->read_word(D + 0x2400u + (uint32_t)(i * 4));
            if (mp == 0) break;
            uint16_t cls = cpu->read_half(mp + 0x36u);
            if (cls < 83 || cls > 87) continue;
            int32_t mx = (int32_t)cpu->read_word(mp + 0xCu);
            int32_t my = (int32_t)cpu->read_word(mp + 0x10u);
            int32_t mz = (int32_t)cpu->read_word(mp + 0x14u);
            int32_t dx = mx - cam_x;
            int32_t dy = my - cam_y;
            int32_t dz = mz - cam_z;
            /* Match asm: at = dx/4, v0 = -dy/4, v1 = -dz/4 */
            int32_t a_t = dx >> 2;
            int32_t v0 = -dy >> 2;  // arithmetic shift of negation
            int32_t v1 = -dz >> 2;
            /* Render distance from byte 0x50 */
            uint8_t flag_lo = (uint8_t)cpu->read_byte(mp + 0x50u);
            int32_t a0 = (int32_t)((int8_t)flag_lo) << 8;
            /* Cube test */
            int cube_pass = 1;
            if (a_t + a0 <= 0 || a_t - a0 >= 0) cube_pass = 0;
            if (v0 + a0 <= 0 || v0 - a0 >= 0) cube_pass = 0;
            if (v1 + a0 <= 0 || v1 - a0 >= 0) cube_pass = 0;
            /* MVMVA cone test inputs: IR1=v0, IR2=v1, IR3=at */
            int64_t MAC1 = (int64_t)R11 * v0 + (int64_t)R12 * v1 + (int64_t)R13 * a_t;
            int64_t MAC3 = (int64_t)R31 * v0 + (int64_t)R32 * v1 + (int64_t)R33 * a_t;
            int64_t abs_x = MAC1 < 0 ? -MAC1 : MAC1;
            int64_t cone_x = 4 * (abs_x - 102) - 3 * (MAC3 + 77);
            fprintf(stderr,
                    "  moby 0x%08X cls=%u dxyz=(%d,%d,%d) cube=%s "
                    "MAC1=%lld MAC3=%lld coneX_a1=%lld %s\n",
                    mp, cls, dx, dy, dz, cube_pass ? "PASS" : "CULL",
                    (long long)MAC1, (long long)MAC3, (long long)cone_x,
                    cone_x >= 0 ? "CULL" : "PASS");
        }
        s_in_a2c = 0;
        /* fall through to call original */
    }

    static int s_in_traced_call = 0;
    if (addr == 0x80023AC4u && !s_in_traced_call) {
        s_in_traced_call = 1;
        openpete_trace_writes_begin(cpu);
        func_80023AC4(cpu);
        openpete_trace_writes_end(cpu);
        s_in_traced_call = 0;
        return 1;
    }

    return 0;
}

/* The tables of the game's other TUs the descriptor carries. */
extern const openpete_game_identity_t spyro1_game_identity;   /* gen/game_identity.c */
extern const game_layout_t            spyro1_game_layout;     /* renderer/layout.c */
extern const ui_game_t                spyro1_ui_game;         /* renderer/layout.c */
extern const terrain_game_t           spyro1_terrain_game;    /* renderer/terrain_game.c */
extern const moby_game_t              spyro1_moby_game;       /* renderer/moby_game.c */
extern const player_game_t            spyro1_player_game;     /* renderer/player_game.c */
extern const freecam_game_desc        spyro1_freecam_desc;    /* features/freecam.c */
extern const game_orchestrator_t      spyro1_orchestrator;    /* renderer/reextract.c */
extern const openpete_feature_list_t  spyro1_feature_list;    /* feature_list.c */
extern const struct psyq_game_hooks   spyro1_psyq_hooks;      /* psyq_hooks.cpp */
void spyro1_interp_register(void);                             /* renderer/interp_regions.c */

/* Spyro 1's mod-layer descriptor (mods/mod_game_desc.c). */
const game_mod_desc_t* spyro1_mod_desc(void);
/* The sfx pack census (sfx_census.cpp). */
int spyro1_dump_sfx_pack(const char* cue_path, const char* out_dir, const char* extra_wads);

/* The known disc's asset input hashes (FNV-1a 64): each level's sky
 * payload in level-table order, the glyph font bytes, and the logo's
 * texel rows followed by its palette. */
static const uint64_t k_sky_hashes[] = {
    0xb2b5ab8f13426fa9u, /* artisans */
    0x37e641ecb2e863f2u, /* stone_hill */
    0x89da4f536a31e38bu, /* dark_hollow */
    0x54a00a06f8be399cu, /* town_square */
    0x7a73389736fa774bu, /* toasty */
    0x5aa500756af46e1au, /* sunny_flight */
    0xc9fc4c9e476131f3u, /* peace_keepers */
    0xc07e7e17ebb47d7fu, /* dry_canyon */
    0x249659ee15a6ccd0u, /* cliff_town */
    0xb9621a307b42eaacu, /* ice_cavern */
    0xa9cd3a4c923333beu, /* doctor_shemp */
    0x8b07073b84d969cbu, /* night_flight */
    0x2bf54973bbbc2d68u, /* magic_crafters */
    0x23f890f6fa3dc304u, /* alpine_ridge */
    0xaca98d8a27b6d342u, /* high_caves */
    0xa4323fe0c11a9afdu, /* wizard_peak */
    0x809abc9e25c2c994u, /* blowhard */
    0xdeb3c8aae27f1814u, /* crystal_flight */
    0x8ab9ec328dff66feu, /* beast_makers */
    0xa7ffb63fc84439d9u, /* terrace_village */
    0xaa34117d87616adbu, /* misty_bog */
    0x80369f819e0bd4feu, /* tree_tops */
    0xee9992dfca02781cu, /* metalhead */
    0x5f6f614f554297b9u, /* wild_flight */
    0x9d909ba36eb59372u, /* dream_weavers */
    0x255f0c8b6a27ca80u, /* dark_passage */
    0xb8b7796cb38eb3bdu, /* lofty_castle */
    0xa407e52f803c446fu, /* haunted_towers */
    0x136d3258d7c8f847u, /* jacques */
    0x33a49f434c1db5a3u, /* icy_flight */
    0x33319b7dadc7e4f2u, /* gnorc_gnexus */
    0x85ee94dbcdad6456u, /* gnorc_cove */
    0x6430ac207564e506u, /* twilight_harbor */
    0x47ecc5289d99b9b6u, /* gnasty_gnorc */
    0xc5c2ab90a4b720a1u, /* gnastys_loot */
};
static const game_asset_hashes_t k_asset_hashes[] = {
    {"fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37",
     k_sky_hashes, (int)(sizeof k_sky_hashes / sizeof k_sky_hashes[0]),
     0xf1e49eb6425f8dafu, 0x88f553ee53598b65u},
};
static const game_assets_desc_t k_assets = {
    .levels         = &spyro1_levels,
    .sky_form       = GAME_SKY_LEVEL_HEADER,
    .sky_wad_base   = 10,      /* Artisans: each level holds two entries */
    .sky_components = 5,       /* texture, environment, occlusion, special surface, collision */
    /* WAD entry 1: a raw 512x256-halfword VRAM image of (512,256); the
     * palette is its row 255, the drawn region 255x128 at its origin. */
    .logo           = { 1, -1, 0u, 261120u, 255u, 128u },
    .discs          = k_asset_hashes,
    .disc_count     = (int)(sizeof k_asset_hashes / sizeof k_asset_hashes[0]),
};

static const game_host_desc_t k_host = {
    .ui              = &spyro1_ui_game,
    .terrain         = &spyro1_terrain_game,
    .moby            = &spyro1_moby_game,
    .player          = &spyro1_player_game,
    .freecam         = &spyro1_freecam_desc,
    .orchestrator    = &spyro1_orchestrator,
    .interp_register = spyro1_interp_register,
    .assets          = &k_assets,
};

/* The builds a disc of this game can hold (disc_builds.c). */
extern const openpete_disc_build_row_t spyro1_disc_builds[];

/* The README-pinned retail NTSC-U disc, labelled by name in the library. */
static const openpete_launcher_known_disc k_known_discs[] = {
    {"fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37",
     "Spyro the Dragon (NTSC-U)"},
};

/* The bug-report replay sites: the GamestateDraw wait of live gameplay and
 * the two title-screen waits (GamestateDraw sends each game state to its
 * own VSync loop; the others are not reached by a cold boot or the skip). */
static const openpete_park_site_t k_replay_sites[] = {
    { 0x8001F0E0u, 0x801FFFC0u },
    { 0x8007DD1Cu, 0x801FFF70u },
    { 0x8007DD54u, 0x801FFF70u },
};

/* pad_read_pc: PadVSync (spyro-1/src/gamepad.c) @ 0x80053C68, the game's
 * once-per-VSync consumption of g_PadBuffer: it reads status, size_type,
 * inputs[] and sticks and folds them into g_Pad. Every other reader in the
 * game works from g_Pad, so this entry is where the host's latched sample
 * becomes the tick's input. PadDemoUpdate (0x800539FC) also touches the
 * buffer but only on the demo-record path, which is not a play path.
 *
 * consume_pc: GamestateUpdate (spyro-1/src/gamestates/update.c) @
 * 0x8003385C, called once per pass of main's loop (src/main.c), which then
 * resets g_UnprocessedFrames: the tick consumes the m_BufferedInputs ring
 * PadVSync filled once per VBL. Its entries key a consume-keyed bk2's rows.
 *
 * load_delay_default (unset, 0): deferred reads never complete in the title
 * demo load's LoadLevel spin or CDLoadSync; goldens and bk2s carry 0.
 *
 * sony_image_base: the g_SonyImage / D_8006FCF4 scratch pool. The
 * recompiler rewrites a `lui` + `addiu` pair composing an address in the
 * recomp.conf SONY_IMAGE_WINDOW, [0x8006FCF4, 0x80072E94), to
 * `psx_runtime_g_sony_image_base + offset`; at the canonical value the
 * expression equals the original address. The
 * terrain renderer reads it (games/common/renderer/terrain.c).
 *
 * No sub-area targets: every --skip-to-area value is refused. The level
 * table is features/levelwarp.c's, shared with the dev-overlay warp
 * dropdown. */
const openpete_game_runtime_t spyro1_game_runtime = {
    .identity             = &spyro1_game_identity,
    .layout               = &spyro1_game_layout,
    .disc_layout          = &k_disc_layout,
    .pad_read_pc          = 0x80053C68u,
    .sony_image_base      = 0x8006FCF4u,
    .loop_anchor_pc       = 0x8001ED5Cu,
    .known_discs          = k_known_discs,
    .known_disc_count     = (int)(sizeof k_known_discs / sizeof k_known_discs[0]),
    .disc_builds          = spyro1_disc_builds,
    .consume_pc           = 0x8003385Cu,
    .replay_sites         = k_replay_sites,
    .replay_site_count    = (int)(sizeof k_replay_sites / sizeof k_replay_sites[0]),
    .on_boot              = spyro1_on_boot,
    .on_frame_pre         = spyro1_on_frame_pre,
    .on_state_loaded      = spyro1_on_state_loaded,
    .state_extra_size     = spyro1_state_extra_size,
    .state_extra_save     = spyro1_state_extra_save,
    .state_extra_load     = spyro1_state_extra_load,
    .is_gameplay_live     = spyro1_is_gameplay_live,
    .set_skip_to_level    = spyro1_set_skip_to_level,
    .resolve_level_arg    = spyro1_level_resolve,
    .set_skip_to_cutscene = spyro1_set_skip_to_cutscene,
    .resolve_cutscene_arg = spyro1_cutscene_resolve,
    .validate_cheats_arg  = spyro1_cheats_validate,
    .surface              = spyro1_surface,
    .features             = &spyro1_feature_list,
    .host                 = &k_host,
    .psyq                 = &spyro1_psyq_hooks,
    .override_dispatch    = spyro1_override_dispatch,
    .mod_desc             = spyro1_mod_desc,
    .mod_disc_configure   = spyro1_mod_wad_configure,
    .dump_sfx_pack        = spyro1_dump_sfx_pack,
};
