#ifndef OPENPETE_CONFIG_H
#define OPENPETE_CONFIG_H

/*
 * openpete.toml - the single layered config file, and the only place a
 * persistent setting is written.
 *
 * PRECEDENCE (ascending - later wins):
 *
 *    0  built-in defaults
 *    1  openpete.toml   global scope
 *    2  openpete.toml   [game.<id>]
 *    3  openpete.toml   [disc.<SERIAL>]
 *    4  openpete.toml   [disc.<sha256>]
 *    5  openpete.toml   [saved]           (written by the in-game menus)
 *    6  the real process environment
 *    7  <movie>.bk2.args sidecar
 *    8  the command line as typed
 *    9  an in-game edit (a live mutation, not a layer)
 *
 * Tiers 1-4 subdivide by FILE (exe-dir < user-dir) and then by SPELLING
 * (bare keys < [env] < [args]) - scope is the major sort key, notation
 * the minor one, and in-file spellings rank as their real-world
 * counterparts do so that moving a line between the file and the shell
 * never changes which of two settings wins.
 *
 * The mechanism that makes the ordering real is CANONICAL FLATTENING:
 * a knob's winning layer is decided here, and only that one spelling is
 * materialized (an argv token or a setenv). Splicing and setenv-ing every
 * layer would hand the args-vs-env tiebreak back to each knob's own
 * sentinel-unset consumer, which is blind to scope - a low-specificity
 * [args] would beat a high-specificity [env].
 */

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- boot ---------------------------------------------------------- */

/** @brief Load the config files, resolve every layer, and materialize each knob's winning spelling.
 *
 * Seeds the keymap, scans argv for --no-config, --config-report,
 * --keys-report and --config (OPENPETE_CONFIG when --config is absent;
 * the value "none" disables the files), reads the exe-dir and per-user
 * openpete.toml (or only the --config file), harvests the global, game,
 * disc-serial, disc-sha and [saved] scopes, resolves the keymap, and
 * materializes the winners: `[env]` winners are set in the process
 * environment (a variable the real environment already carries still
 * wins), and flag winners are spliced into a new argv after argv[0], left
 * of the caller's arguments, so a typed flag still overrides them.
 *
 * The full stage: called once, from openpete_options_parse after the game
 * is identified, before any getenv consumer of a game or disc scope value
 * runs, because features cache their env reads on first use. A
 * device-creation knob of class CFG_DEVICE_FIXED or the `disc` knob set in
 * a game or disc scope is not applied and is listed by --config-report.
 * @param argc Argument count, including a bk2 sidecar splice.
 * @param argv Arguments; may carry the bk2 sidecar tokens left of the typed ones.
 * @param typed_argc Count of the arguments as typed, so the config report can label a sidecar token apart from a typed one; both outrank every file tier.
 * @param out_argc Receives the spliced argument count when the return is 1.
 * @param out_argv Receives the spliced argv when the return is 1; heap-owned and retained for the process lifetime.
 * @return 1 when tokens were spliced, 0 when none were, -1 on a config error that aborts startup (already printed to stderr). */
int openpete_config_apply(int argc, char** argv, int typed_argc,
                          int* out_argc, char*** out_argv);

/** @brief Resolve the tiers that hold no game or disc scope, writing nothing to the process environment.
 *
 * The game-neutral stage, run before the game is identified. Reads the
 * same files and launch tiers as openpete_config_apply and harvests the
 * global and [saved] scopes, but not [game.*], [disc.*] or any [keys]
 * table. Flag winners are spliced into a new argv as openpete_config_apply
 * splices them; [env] winners are kept in memory and read back through
 * openpete_config_env, never setenv. openpete_config_apply then runs once,
 * after identification, from every tier.
 * @param argc Argument count, including a bk2 sidecar splice.
 * @param argv Arguments.
 * @param typed_argc Count of the arguments as typed.
 * @param out_argc Receives the spliced argument count when the return is 1.
 * @param out_argv Receives the spliced argv when the return is 1; retained for the process lifetime.
 * @return 1 when tokens were spliced, 0 when none were, -1 on a config error (already printed). */
int openpete_config_apply_neutral(int argc, char** argv, int typed_argc,
                                  int* out_argc, char*** out_argv);

/** @brief The value of env var `name`: the process environment, else the game-neutral stage's [env] winner.
 *
 * After openpete_config_apply the environment holds every winner, so this
 * is getenv; before it, the game-neutral stage's winners are visible here
 * without having been written.
 * @param name The variable.
 * @return The value, or NULL when neither holds it. */
const char* openpete_config_env(const char* name);

/** @brief The value a device-creation knob resolved to from the tiers without a game or disc scope.
 *
 * `spelling` is a flag ("--vsync") or an env var ("OPENPETE_RHI_DEPTH_D24")
 * of the device-creation set (config_schema.h cfg_schema_device_class).
 * The winner is taken over the launch tiers, the real environment, the
 * global scopes and [saved]; when the winning spelling of a two-spelling
 * knob is the other one, NULL is returned, so the caller reads each
 * spelling in its own vocabulary. Valid after either stage.
 * @param spelling The flag or env var.
 * @return The value, or NULL when no such tier sets it. */
const char* openpete_config_device_value(const char* spelling);

/** @brief Return whether some [game.*] or [disc.*] scope of a loaded file could turn a boolean knob off for a game not yet identified.
 *
 * A scope ranks below [saved], the real environment, the bk2 sidecar and
 * the command line, so a knob any of those tiers sets in the current
 * resolution is decided without the scopes and the answer is 0. Otherwise
 * every [game.<id>] and [disc.<key>] table of each loaded file is read,
 * whatever game it names, for the knob's bare key, its [env] variable and
 * its [args] flag; a value that is false, "off" or a number atoi reads as 0
 * (an [args] = false included) answers 1. Valid after either stage; the
 * game-neutral stage's caller uses it to decide before the pick what every
 * game could resolve.
 * @param knob_id Schema knob id.
 * @return 1 when a scope could turn the knob off, else 0 (also for an
 *         unknown id or with --no-config). */
int openpete_config_scopes_may_disable(const char* knob_id);

/** @brief The number of full-stage materializations this process ran (openpete_config_apply calls).
 * @return The count; 1 after a normal boot. */
int openpete_config_materializations(void);

/** @brief Print a diagnostic to stderr unless the same text was printed before.
 *
 * Both stages harvest the global scopes, so a warning about one of their
 * values would otherwise print twice. The option parser's warnings use it
 * for the same reason.
 * @param fmt printf format. */
void openpete_config_diag(const char* fmt, ...);

/** @brief Name the disc image whose [disc.*] scopes the next openpete_config_apply harvests.
 *
 * The identification result: the disc that identified the game (argv,
 * the library entry the player or the run selected). Passed to the
 * disc-identity hook; NULL or "" names none.
 * @param disc_path The image path. */
void openpete_config_set_identified_disc(const char* disc_path);

/** @brief Label the identification source for --config-report ("disc", "library", "selector", "executable").
 * @param source The label; NULL clears it. */
void openpete_config_set_identity_source(const char* source);

/** @brief Set the game id that selects the [game.<id>] scope ("spyro-1").
 *
 * Call before openpete_config_apply. An empty or unset id matches no
 * scope; the id is truncated to 63 characters.
 * @param id Game id; NULL clears it. */
void openpete_config_set_game_id(const char* id);

/* How the [disc.*] scopes get their keys. openpete_config_apply calls this
 * hook with the identified disc (openpete_config_set_identified_disc, NULL
 * when none) to learn that image's serial and/or content hash. Cheap by contract: an ISO serial read, or a library
 * entry's already-recorded hash - never an image hash, which would mean
 * digesting ~500 MB on every launch. Fills `serial` / `sha` with "" for
 * whatever it cannot resolve; returns 0 if it resolved anything.
 *
 * Registered by the game before openpete_config_apply. A function pointer
 * rather than a weak symbol: MinGW's PE/COFF linker mis-resolves weak
 * overrides across static libraries (the same reason the game entry
 * registers its other game hooks explicitly). Unregistered = no [disc.*] scopes. */
typedef int (*openpete_disc_identity_fn)(const char* disc_path,
                                         char* serial, size_t serial_cap,
                                         char* sha, size_t sha_cap);
/** @brief Register the hook that names the booting disc image's serial and hash.
 *
 * Applies to the next openpete_config_apply; the hook is described at
 * openpete_disc_identity_fn. Without one no [disc.*] scope is read.
 * openpete_config_reset clears it.
 * @param fn The hook, or NULL to clear it. */
void openpete_config_set_disc_identity_fn(openpete_disc_identity_fn fn);

/** @brief Clear the resolver's state so the next openpete_config_apply starts from nothing.
 *
 * Frees the parsed documents and clears the file paths, the contribution,
 * token, decision and saved tables, the save target, the disc scopes, the
 * --no-config and --config-report latches, the disc-identity hook, the
 * identified disc and source, the printed-diagnostic memory and the
 * materialization count, and drops the portable directive. The game id and
 * the keymap are kept; the spliced tokens are not freed. Each stage clears
 * its own resolution state on entry, so a caller resets only to forget the
 * identification. */
void openpete_config_reset(void);

/* ---- diagnostics --------------------------------------------------- */

/** @brief Print the config files, scopes, write target, directory layout and knob resolution table.
 *
 * The table lists every knob any layer touched with its value, winning
 * layer and the layers it shadowed, then the unshadowed contributions
 * with no schema row (raw flags and debug env vars) below the real
 * environment tier. The game mains call it for --config-report, printing
 * to stdout and exiting 0.
 * @param out Destination stream; NULL prints to stderr. */
void openpete_config_report(FILE* out);

/** @brief Return one of the config files the layered config reads: 0 the exe directory's openpete.toml, 1 the user config directory's.
 *
 * The path is returned whether or not the file exists.
 * @param i 0 or 1.
 * @return The path, or NULL when that layer resolved none. */
const char* openpete_config_file_path(int i);

/** @brief Name the two config file tiers in place of the exe directory's and the per-user openpete.toml.
 *
 * Called before the options parse. Each named file is read when it
 * exists; a missing one is skipped and nothing is created. The per-user
 * tier, when present, is the save target. --config and --no-config still
 * take precedence.
 * @param exe_dir_file The exe-directory tier's file, or NULL or "" for none.
 * @param user_file The per-user tier's file, or NULL or "" for none. */
void openpete_config_tier_files(const char* exe_dir_file, const char* user_file);

/** @brief Return 1 when openpete_config_apply saw --config-report in argv, else 0.
 *
 * No caller reads it; the game mains test openpete_options_t.config_report
 * instead.
 * @return 1 when --config-report was passed, else 0. */
int openpete_config_report_requested(void);

/** @brief Source label of a token the config spliced into argv ("game.spyro-1.args").
 *
 * The flag parser prints it for an unknown flag so the message names the
 * config scope rather than the command line, and keeps the default for a
 * refused config value where a typed one fails the parse. Tokens match by
 * pointer only: the spliced argv holds the resolver's own token pointers,
 * and a typed token equal in text to a spliced one is not spliced.
 * @param token An argv token.
 * @return The label, or NULL when the token is not a spliced one. */
const char* openpete_config_token_source(const char* token);

/* ---- provenance (the M overlay / O menu) ---------------------------- */

/** @brief The layer that won a knob, as a display string.
 *
 * The winning contribution's origin: "cli", "bk2 sidecar", "env" (the real
 * environment), "saved", or a file scope with its spelling ("global",
 * "game.spyro-1.env", "disc.SCUS-94228.args"). "default" when no layer set
 * the knob or the id is unknown.
 * @param knob_id Schema knob id.
 * @return The label, never NULL. */
const char* openpete_config_knob_layer(const char* knob_id);

/** @brief Whether a knob's winner is a launch tier: the real environment, the bk2 sidecar or the command line.
 *
 * A saved value does not take effect on the next launch while that
 * override is present. The overlay shows the same condition as a "pinned"
 * badge through openpete_config_knob_pin_spelling.
 * @param knob_id Schema knob id.
 * @return 1 when pinned, 0 otherwise or for an unknown id. */
int openpete_config_knob_pinned(const char* knob_id);

/** @brief The launch-tier spelling that pinned a knob ("--lod", "$OPENPETE_MODS").
 *
 * @param knob_id Schema knob id.
 * @return The spelling, or NULL when the knob is not pinned. */
const char* openpete_config_knob_pin_spelling(const char* knob_id);

/* ---- persistence (the in-game menus) -------------------------------- */

/** @brief Record a knob's value in the in-memory [saved] zone.
 *
 * An existing entry for the knob is overwritten; a new one is appended.
 * Nothing reaches disk until openpete_config_save_flush.
 * @param knob_id Schema knob id.
 * @param value Value in the knob's CLI vocabulary.
 * @return 0, or -1 for a NULL argument, an id with no schema row, or a full
 *         table (CFG_MAX_SAVED entries). */
int openpete_config_save_knob(const char* knob_id, const char* value);

/** @brief Write the in-memory [saved] zone to the save target.
 *
 * Everything above the managed marker is copied through byte for byte, so
 * hand-written config and its comments survive; a file without the marker
 * is copied whole. The marker, a note and a [saved] table of every saved
 * knob with a schema row, each written as its knob's type, follow, then a
 * [saved.keys.<domain>] table per domain holding the saved bindings (arrays
 * of strings with backslash and quote escaped; a mod.<id> row one string).
 * The file is written to a temporary beside the target and renamed over it
 * (op_atomic_file_commit), so a failed write, sync or rename leaves the
 * previous file whole. A file whose text above the marker holds a `[saved`
 * table header is refused unwritten: the zone would define the table twice.
 * @return 0 on success, -1 when there is no save target, the source file
 *         cannot be buffered, a [saved] table sits above the marker, the
 *         temporary cannot be opened, or the commit fails. */
int openpete_config_save_flush(void);

/** @brief Store a [saved.keys.<domain>] row in the in-memory zone, replacing the row of the same action.
 *
 * Nothing reaches disk until openpete_config_save_flush.
 * @param domain "host", "pad".."pad4" or "mod.<id>".
 * @param action the action id or mod binding name.
 * @param toks the binding tokens in the [keys] spelling.
 * @param n their count; 0 stores an unbinding row.
 * @return 0, or -1 for a NULL argument, a full table or an over-long name. */
int openpete_config_save_binding(const char* domain, const char* action,
                                 const char* const* toks, int n);

/** @brief Remove a [saved.keys.<domain>] row from the in-memory zone.
 *
 * Nothing reaches disk until openpete_config_save_flush.
 * @param domain the row's domain.
 * @param action the row's action.
 * @return 1 when a row was removed, 0 when the zone held none. */
int openpete_config_unsave_binding(const char* domain, const char* action);

/** @brief Drop every saved knob and binding and rewrite the save target with an empty managed zone.
 *
 * @return openpete_config_save_flush's result. */
int openpete_config_reset_saved(void);

/** @brief Remove a knob's entry from the in-memory [saved] zone.
 *
 * Nothing reaches disk until openpete_config_save_flush.
 * @param knob_id Schema knob id.
 * @return 1 when an entry was removed, 0 when the zone held none. */
int openpete_config_unsave_knob(const char* knob_id);

/** @brief The value a knob takes from the layers below [saved] that every game reads: the global tier of either file.
 *
 * The highest-ranked global contribution decides. [saved] sits above the
 * game and disc tiers, so a comparison under the active game's scope would
 * describe that game only; a knob the active game's or disc's scope sets is
 * reported as not comparable.
 * @param knob_id Schema knob id.
 * @param buf Receives the value in the flag's vocabulary when 1 is returned.
 * @param cap Size of @p buf.
 * @return 1 with the value in @p buf; 0 when no global layer sets the knob
 *         (the built-in default decides); -1 for an unknown id, a knob a
 *         game or disc scope sets, or a global winner spelled in [env] or
 *         withheld by [args] = false. */
int openpete_config_knob_value_below_saved(const char* knob_id, char* buf, size_t cap);

/** @brief Whether the resolver chose a file for the [saved] zone.
 *
 * @return 0 under --no-config or when no config location is writable, else 1. */
int openpete_config_has_save_target(void);

/** @brief The file the managed [saved] zone is written to, for UI display.
 *
 * openpete_config_apply sets it to the write target: the --config file,
 * the executable directory's openpete.toml under a portable layout, else
 * the per-user openpete.toml.
 * @return The path, or "(none)" when there is no write target; never NULL. */
const char* openpete_config_save_path(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_CONFIG_H */
