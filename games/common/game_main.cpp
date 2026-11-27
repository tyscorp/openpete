// games/common/game_main.cpp - every game's entry (openpete_game_main,
// game_build.h), run by the process entry (game_entry.cpp) once the game is
// selected and installed.
//
// Parse options, resolve the disc through the common launcher, register
// the mod layer and the recompiled module, compile stale mods on a launcher
// wait, hand off to core's run loop. What differs between games is reached
// through the runtime descriptor (the mod-layer descriptor, the mod
// services that read the disc, the sfx census) and the build record (the
// first-boot pipeline, the carve).

#include "openpete.h"
#include "openpete_config.h"
#include "openpete_fatal.h"
#include "openpete_disc_build.h"
#include "openpete_identify.h"
#include "openpete_log.h"
#include "openpete_boot_timing.h"
#include "openpete_keymap.h"
#include "openpete_launcher.h"
#include "openpete_surface.h"
#include "game_build.h"
#include "game_pick.h"
#include "game_extras.h"
#include "game_host_desc.h"
#include "openpete_mods.h"
#include "openpete_paths.h"
#include "openpete_psx.h"
#include "psyq/psyq_game_hooks.h"
#include "recomp_attach.h"
#include "renderer/orchestrator.h"   /* game_interp_register, game_reextract_init */

#include <cstdio>
#include <string>

/** @brief Tell the player the game cannot start, then hand back the exit code.
 *
 * @param rc The exit code the entry returns.
 * @param err The error's id.
 * @param why What failed, as a sentence for the player.
 * @return rc. */
static int fatal_start(int rc, openpete_error_t err, const char* why) {
    openpete_fatal_message(err, "OpenPete could not start", why);
    return rc;
}

/** @brief Parse the command line and the config stack into opts, telling the player when the settings are rejected.
 *
 * With no identification the parse is the game-neutral one
 * (openpete_options_parse_neutral), which writes nothing to the process;
 * with one it is the one full parse (openpete_options_parse_identified).
 * A launch with no arguments (a double-click) can only fail on the config
 * file or the environment, so only that case shows the message box; a
 * launch with arguments came from a terminal, which shows the parser's
 * message. After a successful parse the box is allowed for every run that
 * is not --headless.
 * @param opts Receives the options.
 * @param argc The argument count.
 * @param argv The arguments.
 * @param id The identification, or NULL for the game-neutral parse.
 * @return OPENPETE_EXIT_OK or the parser's exit code. */
static int parse_options(openpete_options_t* opts, int argc, char** argv,
                         const openpete_ident_t* id) {
    int rc = id ? openpete_options_parse_identified(opts, argc, argv, id,
                                                    game_get_display_name())
                : openpete_options_parse_neutral(opts, argc, argv);
    openpete_fatal_set_interactive(!opts->headless && argc <= 1);
    if (rc != OPENPETE_EXIT_OK)
        return fatal_start(rc, OP_E_CONFIG, "A setting in openpete.toml (or an OPENPETE_ "
                                            "environment variable) was not accepted.");
    openpete_fatal_set_interactive(!opts->headless);
    return OPENPETE_EXIT_OK;
}

/** @brief Name the game from the game-neutral options and an optional library pick: the disc image, else --game, else this executable's game.
 *
 * The disc is --ingest's image or the --disc path; a library boot passes
 * the entry it booted and that entry's recorded game.
 * @param opts The game-neutral options.
 * @param library_game The booted library entry's game ("" when unidentified), or NULL when the library booted nothing.
 * @param id Receives the identification.
 * @return OPENPETE_EXIT_OK or OPENPETE_EXIT_SETUP_ERROR. */
static int identify(const openpete_options_t* opts, const char* library_game,
                    openpete_ident_t* id) {
    openpete_ident_in_t in = {};
    in.selector = opts->game_selector;
    in.disc_path = opts->ingest_path && !library_game ? opts->ingest_path : opts->disc_path;
    in.from_library = library_game != nullptr;
    in.recorded_game = (library_game && *library_game) ? library_game : nullptr;
    in.linked_game = game_get_id();
    return openpete_identify(&in, id);
}

/** @brief The launcher's view of this game: its launcher entry (openpete_game_launcher_entry), which names the game explicitly.
 *
 * @return The entry. */
static const openpete_launcher_game* launcher_game_desc(void) {
    return openpete_game_launcher_entry(openpete_game_build());
}

/** @brief Answer --config-report or --keys-report from the identified resolution, without reading the library.
 *
 * Mod keys are bindings too; the keys report is the audit of what an
 * install responds to, so it registers them first.
 * @param argc The argument count.
 * @param argv The arguments.
 * @param neutral The game-neutral options.
 * @return 0 when the report is written, else the identification's or the parser's exit code. */
static int report_mode(int argc, char** argv, const openpete_options_t* neutral) {
    openpete_ident_t id;
    int rc = identify(neutral, nullptr, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;
    openpete_options_t opts;
    rc = parse_options(&opts, argc, argv, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (opts.config_report) {
        openpete_config_report(stdout);
        return 0;
    }
    openpete_mods_register_key_defaults(&opts);
    openpete_keymap_report(stdout);
    return 0;
}

/** @brief Answer --extract-recomp-inputs: carve the first-boot inputs from the disc with the staged manifest.
 *
 * The carve is the game build's extract_recomp_inputs, the first stage of
 * the first-boot pipeline, reading recomp/manifest/<id>/manifest.toml beside
 * the executable; it runs before any module attach, so it works where no
 * module exists. A build without the carve refuses.
 * @param opts The full resolution.
 * @return 0 when every input was written, else 1. */
static int extract_inputs(const openpete_options_t* opts) {
    const openpete_game_build_t* b = openpete_game_build();
    if (!b->extract_recomp_inputs) {
        std::fprintf(stderr, "openpete: --extract-recomp-inputs: this build has no first-boot "
                             "pipeline for %s (OPENPETE_RECOMP_FIRST_BOOT)\n", b->id);
        return 1;
    }
    char dir[4096];
    if (!opts->disc_path || openpete_exe_dir(dir, sizeof dir) != 0) {
        std::fprintf(stderr, "openpete: --extract-recomp-inputs needs --disc\n");
        return 1;
    }
    const std::string manifest = std::string(dir) + "/recomp/manifest/" + b->id + "/manifest.toml";
    return b->extract_recomp_inputs(opts->disc_path, opts->extract_recomp_inputs_dir,
                                    manifest.c_str()) == 0 ? 0 : 1;
}

/** @brief Run a batch exit mode that needs the identified disc: --extract-recomp-inputs or --dump-sfx-pack.
 *
 * --dump-sfx-pack runs the game's sfx census (the runtime descriptor's
 * dump_sfx_pack; refused for a game without one) and takes the library's
 * most recent entry when no disc is named; the launcher shows no screen,
 * since a batch command must not open a window to ask which version to
 * census.
 * @param argc The argument count.
 * @param argv The arguments.
 * @param neutral The game-neutral options.
 * @return The process exit code. */
static int batch_disc_mode(int argc, char** argv, openpete_options_t* neutral) {
    const bool sfx = !neutral->extract_recomp_inputs_dir;
    int (*const dump_sfx_pack)(const char*, const char*, const char*) =
        openpete_game_runtime()->dump_sfx_pack;
    if (sfx && !dump_sfx_pack) {
        std::fprintf(stderr, "openpete: --dump-sfx-pack: %s has no sfx census\n", game_get_id());
        return 1;
    }
    const char* library_game = nullptr;
    if (sfx && !neutral->disc_path && !neutral->exe_path) {
        int exit_now = 0;
        openpete_launcher_begin(launcher_game_desc(), neutral, 0);
        (void)openpete_launcher_resolve(neutral, nullptr, 0, &exit_now);
        library_game = openpete_launcher_selected_game();
    }
    openpete_ident_t id;
    int rc = identify(neutral, library_game, &id);
    if (rc == OPENPETE_EXIT_OK) rc = openpete_identify_require_linked(&id, game_get_id());
    if (rc != OPENPETE_EXIT_OK) return rc;
    openpete_options_t opts;
    rc = parse_options(&opts, argc, argv, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (!sfx) return extract_inputs(&opts);
    return dump_sfx_pack(opts.disc_path, opts.dump_sfx_pack_dir, opts.dump_sfx_wads) == 0 ? 0 : 1;
}

/** @brief Pick the disc through the library and the launcher, then identify the game.
 *
 * --ingest copies an image in; no disc at all boots the library's entry, or
 * the version selector picks among several; an empty library opens the drop
 * screen. An explicit --disc bypasses the library (dev/CI path). The
 * launcher's window and device open only when a screen is needed.
 * @param neutral The game-neutral options; receives the library's disc.
 * @param module_path Receives the library entry's module path, "" for none.
 * @param module_cap Its capacity.
 * @param id Receives the identification.
 * @param exit_now Set to 1 when the run ends in the launcher (the player quit).
 * @return OPENPETE_EXIT_OK, or the exit code after the player was told. */
static int launcher_identify(openpete_options_t* neutral, char* module_path,
                             size_t module_cap, openpete_ident_t* id, int* exit_now) {
    openpete_launcher_begin(launcher_game_desc(), neutral, !neutral->headless);
    int rc = openpete_launcher_resolve(neutral, module_path, module_cap, exit_now);
    if (rc != OPENPETE_EXIT_OK || *exit_now) return rc;
    rc = identify(neutral, openpete_launcher_selected_game(), id);
    if (rc == OPENPETE_EXIT_OK) rc = openpete_identify_require_linked(id, game_get_id());
    if (rc != OPENPETE_EXIT_OK)
        return fatal_start(rc, OP_E_WRONG_DISC, "The disc does not match the game chosen to run.");
    return OPENPETE_EXIT_OK;
}

/** @brief Register the identified game's mod layer: its mod-layer descriptor, then its mod services that read the disc.
 *
 * Runs once, after identification and before any mod compile or load, so
 * mods override by name against the identified game's table.
 * @param disc_path The identified disc. */
static void register_mod_layer(const char* disc_path) {
    const openpete_game_runtime_t* rt = openpete_game_runtime();
    if (rt->mod_desc) (void)openpete_mods_set_game_desc(rt->mod_desc());
    if (rt->mod_disc_configure) rt->mod_disc_configure(disc_path);
}

/** @brief Attach the recompiled module and compile stale mods, ending the run in the launcher when the player quits during either.
 *
 * The runtime reaches recompiled code only through the recomp-module
 * descriptor: the linked-in module (static linkage) or the one loaded from
 * --recomp-module, the library entry's module, or <tag>-recomp beside the
 * executable (shared linkage). Must precede any dispatch. Only a build with
 * the first-boot pipeline builds a missing module, and never into an
 * explicit --recomp-module path. Every stale enabled source mod then
 * compiles on a launcher task, so openpete_mods_init only loads.
 * @param opts The full resolution.
 * @param library_module_path The library entry's module path, "" for none.
 * @param exit_now Set to 1 when the player quit during the module build or the mod compile.
 * @return OPENPETE_EXIT_OK, or 1 after telling the player the module cannot be attached. */
static int attach_and_compile(const openpete_options_t* opts, const char* library_module_path,
                              int* exit_now) {
    const char* module_override = opts->recomp_module_path;
    if (!module_override && library_module_path[0])
        module_override = library_module_path;
    openpete_boot_mark(OP_BOOT_MODULE_ATTACH_START);
    if (openpete_game_recomp_attach(module_override, opts->disc_path,
                                    opts->recomp_module_path == NULL) != 0) {
        /* A quit during the rebuild ends the run in the launcher. */
        if (openpete_launcher_quit_taken()) {
            *exit_now = 1;
            return OPENPETE_EXIT_OK;
        }
        return fatal_start(1, OP_E_MODULE,
                           "The game code built from the disc could not be loaded or built.");
    }
    openpete_boot_mark(OP_BOOT_MODULE_ATTACHED);
    openpete_mods_prepare(opts, openpete_launcher_progress, openpete_launcher_run_task);
    if (openpete_launcher_quit_taken()) {   /* a quit during the mod compile */
        *exit_now = 1;
        return OPENPETE_EXIT_OK;
    }
    openpete_boot_mark(OP_BOOT_MODS_COMPILED);
    return OPENPETE_EXIT_OK;
}

/** @brief Take the session from the game-neutral options to the hand-off: the launcher's disc, identification, the full parse, the mod layer, the recompiled module and the mod compile.
 *
 * The launcher resolves the disc through the library (the drop screen, the
 * version selector) when none is named and draws the waits (the first-boot
 * module build, the mod compile), opening its window only when one of them
 * has work. The hand-off gives the game the launcher's window, Vulkan
 * device and imgui context in a windowed run (creating them when no screen
 * did), so one OS window and one Vulkan device carry launcher and game.
 * @param argc The argument count.
 * @param argv The arguments.
 * @param neutral The game-neutral options; receives the library's disc.
 * @param opts Receives the full resolution.
 * @param exit_now Set to 1 when the run ends in the launcher.
 * @return OPENPETE_EXIT_OK, or after telling the player:
 *         OPENPETE_EXIT_SETUP_ERROR on a refused identification or a missing
 *         disc, the parser's status, or 1 when the recompiled module cannot
 *         be attached. */
static int start_session(int argc, char** argv, openpete_options_t* neutral,
                         openpete_options_t* opts, int* exit_now) {
    static char library_module_path[4096];
    openpete_ident_t id;
    int rc = launcher_identify(neutral, library_module_path, sizeof library_module_path,
                               &id, exit_now);
    if (rc != OPENPETE_EXIT_OK || *exit_now) return rc;

    /* The one full resolution: the identified game's and disc's scopes
     * join the tiers, and the environment and argv are written once. */
    rc = parse_options(opts, argc, argv, &id);
    if (rc != OPENPETE_EXIT_OK) return rc;
    openpete_game_report_resolution(opts);
    rc = openpete_options_require_exe_source(opts);
    if (rc != OPENPETE_EXIT_OK)
        return fatal_start(rc, OP_E_NO_DISC, "No game disc image was found.");
    /* The disc's build, before the game's code is attached: a disc this
     * game cannot boot is refused with its message, another build is
     * warned about and boots. */
    if (!opts->exe_path && openpete_disc_build_boot_check(opts->disc_path, game_get_exe_filename()) != 0)
        return OPENPETE_EXIT_SETUP_ERROR;

    register_mod_layer(opts->disc_path);
    openpete_boot_mark(OP_BOOT_MODS_STAGED);

    rc = attach_and_compile(opts, library_module_path, exit_now);
    if (rc != OPENPETE_EXIT_OK || *exit_now) return rc;
    return openpete_launcher_handoff(opts);
}

int openpete_game_main(int argc, char** argv) {
    openpete_boot_mark(OP_BOOT_LAUNCHER_START);
    /* abort() does not flush, and the PSYQ stubs use it to surface an
     * unimplemented surface. Unbuffer so that diagnostic survives. */
    setvbuf(stderr, NULL, _IONBF, 0);

    /* The [disc.*] scopes' keys for the identified disc: its serial, and
     * the hash its library entry recorded. */
    openpete_config_set_disc_identity_fn(openpete_library_disc_identity);

    /* OPENPETE_SURFACE_DUMP: write the surface and exit, before any config
     * or env layer runs. */
    openpete_surface_dump_run_if_requested(argv[0]);

    /* The game-neutral tiers, writing nothing to the process. */
    openpete_options_t neutral;
    openpete_options_init(&neutral);
    int rc = parse_options(&neutral, argc, argv, nullptr);
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (neutral.help) return 0;
    if (neutral.config_report || neutral.keys_report)
        return report_mode(argc, argv, &neutral);

    /* The logger and its file mirror come up before the module attach,
     * so a first-boot module build that fails is recorded in the file.
     * Format and file are device-creation knobs: the game-neutral tiers
     * decide them. */
    openpete_log_init(neutral.log_format, neutral.log_level_spec
                                              ? neutral.log_level_spec
                                              : openpete_config_env("OPENPETE_LOG"));
    if (openpete_log_file_open(neutral.log_file ? neutral.log_file
                                                : openpete_config_env("OPENPETE_LOG_FILE")) == 0)
        OPL_INFO_F("log", "log file: %s (build %s %s)",
                   openpete_log_file_path(), __DATE__, __TIME__);

    /* --compile-mods: compile every mod for each game staged under the
     * SDK and exit. No game is identified, so the settings are the
     * game-neutral tiers, materialized once. */
    if (neutral.compile_mods) {
        openpete_ident_t none = {};
        openpete_options_t batch;
        rc = parse_options(&batch, argc, argv, &none);
        if (rc != OPENPETE_EXIT_OK) return rc;
        return openpete_mods_compile_all(&batch) == 0 ? 0 : 1;
    }

    if (neutral.extract_recomp_inputs_dir || neutral.dump_sfx_pack_dir)
        return batch_disc_mode(argc, argv, &neutral);

    openpete_options_t opts;
    int exit_now = 0;
    rc = start_session(argc, argv, &neutral, &opts, &exit_now);
    if (rc != OPENPETE_EXIT_OK || exit_now) {
        openpete_launcher_close();
        return rc;
    }

    /* The gameplay-state region table the oracle hashes (games/common/
     * state_regions.c); explicit, before openpete_run(). */
    game_state_regions_register();

    /* The disc cache loader's boot-order profile (games/common/
     * disc_boot_order.c); before openpete_run() attaches the cache. */
    game_disc_boot_order_register();

    /* The save-header location the window icon is decoded from
     * (openpete_save_icon.h); before openpete_run() hands the icon to the
     * window. */
    game_save_icon_register();

    /* The interp ring's region table and the sub-tick re-extract entry, both
     * before openpete_run() so openpete_interp_init() sees the regions; inert
     * on a game whose TUs are the none defaults. */
    game_interp_register();
    game_reextract_init();

    /* Root counter 2 as the game's boot reads it (the RNG seed source). */
    openpete_psx_set_root_counter2(psyq_game_root_counter2());

    return openpete_run(&opts);
}
