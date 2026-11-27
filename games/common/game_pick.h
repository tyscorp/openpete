// games/common/game_pick.h - the launcher's pick of the game, for an
// executable linking several games whose command line names none.
//
// Runs before the final install: the game-neutral options, the log, the
// library lock, and the launcher over every game's library entries (the
// launch-time module rebuilds, the version selector, the drop screen and its
// ingest), installing each game whose jobs run. The picked entry's game is
// then installed by the process entry, and its entry boots that entry
// (openpete_launcher_resolve). A run whose --disc names a linked game
// (game_build.h) does not come here.

#ifndef OPENPETE_GAME_PICK_H
#define OPENPETE_GAME_PICK_H

#include "game_build.h"
#include "openpete_launcher.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Pick the game this process runs through the launcher, before the final install.
 *
 * Parses the game-neutral options. --help ends the run (the parse printed
 * it); a parse failure ends it after telling the player; an informational
 * or batch mode (--config-report, --keys-report, --compile-mods,
 * --extract-recomp-inputs, --dump-sfx-pack, the surface and tooltip
 * dumps) takes the game --game names when it is linked, else the first
 * linked game, whose entry runs it. Otherwise the log opens, the library
 * lock is taken (a second launcher process ends here with
 * OPENPETE_EXIT_SETUP_ERROR, windowed after showing the refusal on the game
 * page until the next command, openpete_launcher_refuse), and the launcher resolves the disc over every
 * game's entries: the launch-time module rebuilds, an --ingest image, the
 * --game entry or the selector's pick, each pick's game installed and its
 * module built in the launcher (openpete_launcher_resolve). The game
 * recorded on the booted entry is picked, else the --game game, else the
 * first linked game.
 * @param argc The argument count.
 * @param argv The arguments.
 * @param exit_code Receives the process exit status when NULL is returned.
 * @return The game to install, or NULL when the run ends here (--help, a
 *         rejected setting, the library locked by another process, the
 *         player quit the launcher, the library could not be read, the
 *         picked entry's module could not be built without screens). */
const openpete_game_build_t* openpete_game_pick(int argc, char** argv, int* exit_code);

/** @brief Return a linked game's launcher entry: its id, name, known discs, its library module path and its module build, all naming the game explicitly.
 *
 * The pick lists every game's entry in its neutral descriptor; each game's
 * entry (openpete_game_main) begins its launcher with the running game's.
 * @param b The game.
 * @return The entry, or NULL for a game this executable does not link. */
const openpete_launcher_game* openpete_game_launcher_entry(const openpete_game_build_t* b);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_PICK_H */
